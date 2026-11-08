// MeshRenderer's forward geometry: the static / skinned / generic queue walkers (the deferred G-buffer
// reuses the static one), the manual forward passes drawn over the deferred composite, and their pipelines.
#include "MeshRendererInternal.hpp"
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include <Engine/Graphic/DefaultTextures.hpp>
#include <Engine/Graphic/Materials/SurfaceBlendPipeline.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

#include <Common/Core/JobSystem.hpp>

#include <algorithm>
#include <bit>
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
    // The glass declare tests these slots by SceneViewSlot bit: the bit's table name is the parameter's name.
    static_assert( MeshForwardShaderParameters::SceneColorTexture.Name ==
                   ::Desert::Graphic::kPassSceneColorTextureName );
    static_assert( MeshForwardShaderParameters::FrontLayerReflectionTexture.Name ==
                   ::Desert::Graphic::kPassFrontLayerReflectionTextureName );
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
        // Written into @p key (cleared first; its capacity is the caller's) with @p order as the sort scratch, so
        // the per-frame caller (GenericTextureKeyOf) allocates nothing once its strings have grown. The overrides
        // are sorted by (sampler, handle) and the runtime texture, whose part alone carries "=@", goes last: equal
        // sets spell one key in any order, and no two sets spell the same one.
        void WriteGenericTextureKey( const MeshRenderer::GenericMeshRenderData& g, std::string& key,
                                     std::vector<const std::pair<std::string, uint64_t>*>& order )
        {
            key.clear();
            if ( g.SlotMaterial != nullptr )
                return; // the material IS the asset; it is already its own key

            order.clear();
            for ( const auto& texture : g.Overrides().Textures )
                if ( texture.second != 0 )
                    order.push_back( &texture );
            std::sort( order.begin(), order.end(), []( const auto* a, const auto* b ) { return *a < *b; } );
            for ( const auto* const texture : order )
                std::format_to( std::back_inserter( key ), "|{}={}", texture->first, texture->second );
            if ( g.DirectTexture != nullptr && !g.DirectTextureSampler().empty() )
                std::format_to( std::back_inserter( key ), "|{}=@{}", g.DirectTextureSampler(),
                                static_cast<const void*>( g.DirectTexture ) );
        }

        std::string GenericTextureKey( const MeshRenderer::GenericMeshRenderData& g )
        {
            std::string                                          key;
            std::vector<const std::pair<std::string, uint64_t>*> order;
            WriteGenericTextureKey( g, key, order );
            return key;
        }

        void ApplyGenericOverrides( DataDrivenMaterial& material, const MeshRenderer::GenericMeshRenderData& g,
                                    const std::string_view shaderName, const bool logRefused )
        {
            material.ApplyDefaults();
            for ( const auto& [name, value] : g.Overrides().Params )
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
            for ( const auto& [name, handle] : g.Overrides().Textures )
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
            if ( g.DirectTexture != nullptr && !g.DirectTextureSampler().empty() &&
                 !material.SetTexture( g.DirectTextureSampler(), g.DirectTexture ) && logRefused )
                LogRefusedTextureOverride( shaderName, g.DirectTextureSampler() );
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

        for ( std::size_t gi = 0; gi < m_GenericQueue.size(); ++gi )
        {
            GenericMeshRenderData& g = m_GenericQueue[gi];
            g.MaterialRow.reset();
            g.MaterialSurface = {};
            if ( g.Mesh == nullptr )
                continue;
            if ( g.SlotMaterial != nullptr )
            {
                // A per-slot draw's material IS the asset: its values were applied when it loaded, and its row is
                // the asset's (the key the static queue and the card capture name for it too). Its program
                // variants (MaterialService::GetVariant) are built from the same asset and read the same row.
                if ( const auto* const slot = dynamic_cast<const DataDrivenMaterial*>( g.SlotMaterial ) )
                {
                    g.MaterialRow     = RegisterMaterialRow( registry, slot, nullptr, true );
                    g.MaterialSurface = { slot, nullptr };
                }
                continue;
            }
            // A Shader Override draw owns no material: several entities share one per (template x texture set),
            // so each record's values are its own row — the template's defaults, its Overrides.Params and the
            // texture slots of its texture overrides — keyed by its entity part. That is what keeps three spheres
            // differing only in a graph parameter three colours (MAT_ProbeSharedBlock.desce).
            const ResolvedProgram&         resolved = GenericProgramOf( gi ); // resolved once a frame
            const std::shared_ptr<Shader>& shader   = resolved.Program;
            if ( !shader || !shader->IsCompiled() ||
                 !Core::Formats::DrawnByMeshPath( shader->GetProgramMeta().Domain ) )
                continue; // ShaderService named the compile error; BuildGenericDraws names the refused domain
            // The (template x texture set) material by the record's texture-set id, kept with the template's
            // facts: the string key is assembled on the first record of that pair alone.
            DataDrivenMaterial* shared = nullptr;
            if ( resolved.Facts != nullptr )
            {
                auto [row, made] = resolved.Facts->RowMaterials.try_emplace( GenericTextureSetOf( gi ), nullptr );
                if ( made )
                    row->second = SharedGenericMaterial( g.ShaderName(), gi, shader );
                shared = row->second;
            }
            else
            {
                shared = SharedGenericMaterial( g.ShaderName(), gi, shader );
            }
            // The draw-time slices name a refused texture; this row build does not repeat it.
            ApplyGenericOverrides( *shared, g, g.ShaderName(), false );
            const GpuDriven::MaterialRowKey key{ .Material = shared,
                                                 .Owner    = ( static_cast<uint64_t>( g.Entity ) << 32u ) |
                                                          static_cast<uint64_t>( g.MotionPart ) };
            const auto [claimed, fresh] = ClaimFrameMaterialRow( key );
            if ( fresh )
            {
                claimed->Index = registry.SyncMaterialRow( key, shared->GetParamRow() );
            }
            else
            {
                // Two records of one entity part through one (template x texture set): one row, so their values
                // must agree; the second one's would silently take the first one's colours.
                static std::unordered_set<std::string> s_Shared;
                if ( s_Shared.insert( g.ShaderName() ).second )
                    LOG_ERROR( "[MeshRenderer] entity {} part {} submits two '{}' Shader Override draws with one "
                               "texture set; they share one row of the scene material table (the first one's "
                               "values).",
                               g.Entity, g.MotionPart, g.ShaderName() );
            }
            g.MaterialRow     = claimed->Index;
            g.MaterialSurface = { shared, nullptr };
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

        for ( std::size_t gi = 0; gi < m_GenericQueue.size(); ++gi )
        {
            const GenericMeshRenderData& g = m_GenericQueue[gi];
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
            // The dense snapshot's box (m_StaticSnapshot.GenericBounds), built once per static revision.
            if ( g.Mesh != nullptr && !GenericEntryInView( frustum, gi ) )
                continue;

            // Per-slot draws carry their own material (asset params already applied at build);
            // Shader Override draws use a shader-keyed shared material + per-frame overrides.
            // The name is read in place (the material's or the record's), never copied per draw per frame.
            // (A later reassignment of `material` leaves shaderName on the slot's, which outlives the frame.)
            DataDrivenMaterial* material =
                 g.SlotMaterial != nullptr ? dynamic_cast<DataDrivenMaterial*>( g.SlotMaterial ) : nullptr;
            const std::string& shaderName = material != nullptr ? material->GetShaderName() : g.ShaderName();

            // The record's template and its facts (GenericProgramOf: resolved once a frame, the cells once per
            // program): no GetByName or TemplateCellShader per draw per slice.
            const ResolvedProgram& resolved = GenericProgramOf( gi );
            // The resolved (then slice) program in place: both rows outlive the frame, no count per draw.
            const std::shared_ptr<Shader>* shader = &resolved.Program;
            // A shader whose first compile failed is still registered under its name, so GetByName hands
            // it back like any other. Skip its draws silently: the compilation error was already logged
            // once, with the file and line, and repeating it every frame for every mesh would bury it.
            if ( !*shader || !( *shader )->IsCompiled() || g.Mesh == nullptr ||
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
            if ( const auto domain = ( *shader )->GetProgramMeta().Domain;
                 !Core::Formats::DrawnByMeshPath( domain ) )
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
            ProgramFacts& facts           = *resolved.Facts;
            const bool    surfaceOverride = g.SlotMaterial == nullptr && facts.GBufferCell != nullptr;
            const bool    opaque          = facts.Opaque;

            // The slice's program of the template (SliceProgramOf: a cell, a forward permutation or the template
            // itself), cached in its facts with its key and pipeline spec.
            uint32_t sliceIndex = 0;
            MeshPass cellPass   = MeshPass::Forward;
            switch ( slice )
            {
                case GenericSlice::Forward:
                    // An opaque draw is shaded only when this frame's depth prepass admitted it ("Forward: Depth
                    // Prepass", ForwardDepth): the base pass tests against that depth and writes none.
                    if ( opaque && !OpaqueGenericAdmitted( gi ) )
                        continue;
                    // The forward render path: no screen masks (no G-buffer to trace against); the virtual shadow
                    // map in place of the atlas under VirtualMaps (MeshGeometryPass's Declare set the flag).
                    sliceIndex = ForwardSliceIndex( false, false, m_ForwardVirtualLocal );
                    break;
                case GenericSlice::DeferredSurfaceGBuffer:
                    if ( !surfaceOverride )
                        continue;
                    sliceIndex = kSliceGBufferCell;
                    cellPass   = MeshPass::GBuffer;
                    break;
                case GenericSlice::DeferredDepthNormal:
                case GenericSlice::ForwardDepth:
                {
                    if ( !opaque || ( surfaceOverride && slice == GenericSlice::DeferredDepthNormal ) )
                        continue;
                    // The forward prepass draws a surface override through its template's (Static x Depth) cell
                    // (its Forward-slice draw is shaded by the template itself); every other template through its
                    // DepthNormal program.
                    const std::shared_ptr<Shader>& depthNormal =
                         surfaceOverride ? facts.DepthCell : facts.DepthNormalCell;
                    if ( depthNormal == nullptr )
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
                    if ( surfaceOverride )
                    {
                        sliceIndex = kSliceDepthCell;
                        cellPass   = MeshPass::Depth;
                    }
                    else
                    {
                        sliceIndex = slice == GenericSlice::DeferredDepthNormal ? kSliceDepthNormalGBuffer
                                                                                : kSliceDepthNormalPrepass;
                        cellPass   = MeshPass::DepthNormal;
                    }
                    break;
                }
                case GenericSlice::DeferredForward:
                case GenericSlice::DeferredForwardRayTracedSun:
                    if ( surfaceOverride )
                        continue;
                    // An opaque draw is shaded only when this frame's prepass admitted it (both of its pipelines
                    // Ready); until then the prepass counted its compiling pipelines and nothing of it is drawn.
                    if ( opaque && !OpaqueGenericAdmitted( gi ) )
                        continue;
                    sliceIndex = ForwardSliceIndex( slice == GenericSlice::DeferredForwardRayTracedSun,
                                                    m_GenericLocalShadowMasks, m_ForwardVirtualLocal );
                    break;
            }
            const std::shared_ptr<Shader>& templateShader = resolved.Program;
            SliceProgram* const            sliceProgram   = SliceProgramOf( facts, templateShader, sliceIndex );
            if ( sliceProgram == nullptr )
                continue; // its refusal was said when the slice program was asked
            shader = &sliceProgram->Program;
            if ( sliceProgram->Own && !( *shader )->IsCompiled() )
                continue; // a registered program that failed its compile was named at registration
            // The pipeline tracker's and the shared material's name of this program: the permutation is part of
            // it (made once with the slice program, never formatted per draw).
            const std::string& programKey = sliceProgram->Key;

            if ( material != nullptr && sliceProgram->Own )
            {
                // A slot material is a `.demat`: its sibling in this program comes from the service, built from
                // the same asset (same row, same textures), descriptor sets from THIS program's reflection. Asked
                // once per slice program and build revision of the asset (SlotVariantOf).
                material = SlotVariantOf( *material, sliceIndex, cellPass, sliceProgram->Permutation );
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
                // Kept with the slice program by the record's texture-set id (an integer hash): the string key
                // (program key + texture key) is assembled on the first draw of the pair alone.
                auto [kept, made] = sliceProgram->Materials.try_emplace( GenericTextureSetOf( gi ), nullptr );
                if ( made )
                    kept->second = SharedGenericMaterial( programKey, gi, *shader );
                material = kept->second;
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
                // The slice's spec, built with the slice program (SliceProgramOf: the prepass and G-buffer target
                // and depth state included).
                // Kept with the slice program while the pipeline cache's generation stands (SlicePipelineOf),
                // and reported through the tracker row it keeps (SliceRowOf): no spec hashed, no name hashed.
                const auto&                   built = SlicePipelineOf( *sliceProgram );
                MaterialPipelineTracker::Row& row   = SliceRowOf( *sliceProgram );
                if ( built )
                {
                    TrackMaterialPipeline( row, *built.GetValue() );
                }
                else
                {
                    MaterialPipelineTracker::OnFailed( row );
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
                const auto choice = MaterialPipelineTracker::Choose( row );
                if ( slice == GenericSlice::DeferredDepthNormal || slice == GenericSlice::ForwardDepth )
                {
                    if ( !choice.UseOwnPipeline ||
                         !GenericForwardPipelineReady( facts, templateShader, m_GenericPrepassRayTracedSun,
                                                       m_GenericPrepassLocalShadowMasks,
                                                       m_GenericPrepassVirtualLocal ) )
                        continue;
                    // Dense by queue index: one byte per record, the vector's capacity kept across frames.
                    if ( m_GenericPrepassed.size() < m_GenericQueue.size() )
                        m_GenericPrepassed.resize( m_GenericQueue.size(), 0 );
                    m_GenericPrepassed[gi] = 1;
                    // Admitted, so "Deferred: Generic" shades it; its depth and normal come from the DepthPrepass
                    // bins (the "depthPrepass" cull view against this frame's HZB, AttachBinned) when they hold
                    // every visible submesh - the view's CPU walk rule (CpuWalkHidden; the view takes no cluster
                    // cut, so a VG primitive's instances are in its bins like any other).
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
                                  programKey, MaterialPipelineStateName( row.State ) );
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
                        .Push = DrawPushFields{}.PrimitiveIndex( g.GpuPrimitive ).MaterialIndex( d.Row ) } );
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

    void MeshRenderer::TrackMaterialPipeline( MaterialPipelineTracker::Row& row, const GraphicsPipeline& pipeline )
    {
        switch ( pipeline.GetReadiness() )
        {
            case PipelineReadiness::Compiling:
                MaterialPipelineTracker::OnCompiling( row );
                break;
            case PipelineReadiness::Ready:
                MaterialPipelineTracker::OnReady( row );
                break;
            case PipelineReadiness::Failed:
                MaterialPipelineTracker::OnFailed( row );
                break;
        }
    }

    MaterialPipelineTracker::Row& MeshRenderer::SliceRowOf( SliceProgram& slice )
    {
        // Asked once per slice program (its facts are re-made with every code generation and RecompileGeneration);
        // the row's address is stable for the tracker's life, rows are never erased.
        if ( slice.PipelineRow == nullptr )
            slice.PipelineRow = &m_MaterialPipelines.RowOf( slice.Key );
        return *slice.PipelineRow;
    }

    const Common::ResultStr<std::shared_ptr<GraphicsPipeline>>&
    MeshRenderer::SlicePipelineOf( SliceProgram& slice )
    {
        // The cache's answer for the slice's spec, asked again only when an entry may have been dropped
        // (PipelineCache::Generation: a Clear or a hot reload's InvalidateByShader). An insert keeps the kept one.
        PipelineCache& cache      = m_SceneRenderer->GetPipelineCache();
        const uint64_t generation = cache.Generation();
        if ( !slice.Built || slice.BuiltGeneration != generation )
        {
            slice.Built.emplace( cache.GetOrCreateMaterial( slice.Spec ) );
            slice.BuiltGeneration = generation;
        }
        return *slice.Built;
    }

    DataDrivenMaterial* MeshRenderer::SharedGenericMaterial( const std::string& key, const std::size_t index,
                                                             const std::shared_ptr<Shader>& shader )
    {
        // ONE MATERIAL PER (PROGRAM x TEXTURE SET): the key is assembled in place, and the map only ever inserts,
        // so the address answered here is what the callers keep by texture-set id.
        m_GenericMaterialKey.assign( key );
        m_GenericMaterialKey.append( GenericTextureKeyOf( index ) );
        auto& shared = m_GenericMaterials[m_GenericMaterialKey];
        if ( !shared )
            shared = std::make_unique<DataDrivenMaterial>( shader );
        return shared.get();
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
                                                          const std::optional<uint32_t> view,
                                                          const bool                    instanceIds,
                                                          const bool                    sceneInstances )
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
        Core::ShaderVariant permutation = ForwardShadowMaskVariant( *program, false, false, true );
        if ( permutation.IsDefault() )
            return cell;
        // A GPU-instanced list of ids keeps its define, last (SurfaceInstanceIdsVariant, as the cook lists it).
        if ( instanceIds )
            permutation = SurfaceInstanceIdsVariant( std::move( permutation ) );
        // An ISM's cell by GpuScene id keeps its define, last (SurfaceSceneInstancesVariant, as the cook lists
        // it).
        if ( sceneInstances )
            permutation = SurfaceSceneInstancesVariant( std::move( permutation ) );
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

        // THE COMPACTED BINS' CELLS of @p handle (GRASS-BINS-TAIL-1; UE: every permutation a draw can ask for is
        // in the cooked shader map): its GrassBlade cell of each pass a bin draws in
        // (MeshRenderer::kDrawRowsPasses) in SurfaceDrawRowsVariant, the G-buffer one also over the equal-depth
        // permutation (an early-Z admitted template), each over the id list's permutation when @p ids - composed
        // as InstanceDrawPermutation composes them (depth-equal, then ids, then the rows), so the cooked hash is
        // the acquired one. A template without `Usage GrassBlades` has no GrassBlade cell (CellShaderNameOf
        // refuses it): nothing is listed.
        void AppendDrawRowsVariants( const Runtime::MaterialService& materials,
                                     const Runtime::ShaderService& shaders, const ::Common::AssetHandle& handle,
                                     const MeshVertexPath path, const bool ids,
                                     std::set<std::pair<std::string, uint64_t>>& listed,
                                     std::vector<Core::CookedProgramVariant>& variants )
        {
            for ( const MeshPass pass : MeshRenderer::kDrawRowsPasses )
            {
                const auto cell = materials.CellShaderNameOf( handle, path, pass );
                if ( !cell )
                    continue;
                const auto program = shaders.GetByName( cell.GetValue() );
                if ( program == nullptr )
                    continue;
                std::vector<Core::ShaderVariant> under{ Core::ShaderVariant{} };
                if ( pass == MeshPass::GBuffer )
                {
                    const Core::Formats::SurfaceBlendMode blend = program->GetProgramMeta().Blend;
                    if ( blend == Core::Formats::SurfaceBlendMode::Masked ||
                         blend == Core::Formats::SurfaceBlendMode::Opaque )
                        under.push_back( MeshRenderer::SurfaceDepthEqualVariant() );
                }
                for ( Core::ShaderVariant& base : under )
                {
                    if ( ids )
                        base = MeshRenderer::SurfaceInstanceIdsVariant( std::move( base ) );
                    Core::ShaderVariant permutation = MeshRenderer::SurfaceDrawRowsVariant( std::move( base ) );
                    if ( listed.emplace( cell.GetValue(), permutation.Hash() ).second )
                        variants.push_back( { cell.GetValue(), std::move( permutation ) } );
                }
            }
        }
    } // namespace

    std::vector<Core::CookedProgramVariant> MeshRenderer::MaterialProgramVariants(
         const Runtime::MaterialService& materials, const Runtime::ShaderService& shaders,
         std::span<const ::Common::AssetHandle> handles, std::span<const ::Common::AssetHandle> instanceIdHandles )
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
            // The G-buffer cells of a template the deferred early-Z pass can draw (EarlyZAdmits: Masked under
            // EarlyZPass Masked, every Opaque one too under All; never a `.Parallax` one), in the equal-depth
            // permutation the G-buffer draws them in after it (DepthEqualCell, BuildGpuInstancedDraws): per object and
            // auto-batched / ISM statics, skinned meshes, GPU-instanced sections and blades.
            for ( const MeshVertexPath path : { MeshVertexPath::Static, MeshVertexPath::Instanced,
                                                MeshVertexPath::Skinned, MeshVertexPath::GrassBlade } )
            {
                const auto cell = materials.CellShaderNameOf( handle, path, MeshPass::GBuffer );
                if ( !cell )
                    continue;
                const auto program = shaders.GetByName( cell.GetValue() );
                if ( program == nullptr )
                    continue;
                const Core::Formats::SurfaceBlendMode blend = program->GetProgramMeta().Blend;
                if ( blend != Core::Formats::SurfaceBlendMode::Masked &&
                     blend != Core::Formats::SurfaceBlendMode::Opaque )
                    continue;
                Core::ShaderVariant permutation = SurfaceDepthEqualVariant();
                // The cell's `.Binned` twin too (Static, Instanced, Skinned: the binned paths), the G-buffer bins'
                // equal-depth twin after their depth twin (MeshRenderer::EarlyZBinTwinsOf). A `.Parallax` cell is
                // not admitted (EarlyZAdmitsCell), so its binned one is never asked for.
                if ( path != MeshVertexPath::GrassBlade && !IsParallaxCell( cell.GetValue() ) )
                {
                    const std::string binned = BinnedCellOf( cell.GetValue() );
                    if ( shaders.GetByName( binned ) != nullptr &&
                         listed.emplace( binned, permutation.Hash() ).second )
                        variants.push_back( { binned, permutation } );
                    // Its `.VirtualGeometry` twin (Static, Instanced), the G-buffer clusters' equal-depth twin
                    // after their `.Depth.Binned.VirtualGeometry` one (EarlyZBinTwinsOf, GRASS-VIS-12).
                    const std::string cluster = VirtualGeometryCellOf( binned );
                    if ( shaders.GetByName( cluster ) != nullptr &&
                         listed.emplace( cluster, permutation.Hash() ).second )
                        variants.push_back( { cluster, permutation } );
                }
                if ( listed.emplace( cell.GetValue(), permutation.Hash() ).second )
                    variants.push_back( { cell.GetValue(), std::move( permutation ) } );
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
            // The compacted bins' blade cells over the full list (a section or cascade without a list of ids).
            AppendDrawRowsVariants( materials, shaders, handle, MeshVertexPath::GrassBlade, /*ids*/ false, listed,
                                    variants );
        }
        // The cells a GPU-instanced id list is drawn through, for the materials an id list draws with only
        // (@p instanceIdHandles: the grass types' materials, LandscapeGrassTypeService::InstanceIdListMaterials).
        for ( const ::Common::AssetHandle& handle : instanceIdHandles )
        {
            // Its cells a GPU-instanced id list is drawn through (kInstanceIdsPaths x kInstanceIdsPasses: the
            // camera's list and a cascade's own one, BuildGpuInstancedDraws / BuildGpuInstancedShadowDraws), each
            // in SurfaceInstanceIdsVariant; the G-buffer one also over the equal-depth permutation (an early-Z
            // admitted template, as above), the forward one also over the virtual shadow map's
            // (ForwardVirtualCell).
            for ( const MeshVertexPath path : kInstanceIdsPaths )
                for ( const MeshPass pass : kInstanceIdsPasses )
                {
                    const auto cell = materials.CellShaderNameOf( handle, path, pass );
                    if ( !cell )
                        continue;
                    const auto program = shaders.GetByName( cell.GetValue() );
                    if ( program == nullptr )
                        continue;
                    std::vector<Core::ShaderVariant> under{ Core::ShaderVariant{} };
                    if ( pass == MeshPass::GBuffer )
                    {
                        const Core::Formats::SurfaceBlendMode blend = program->GetProgramMeta().Blend;
                        if ( blend == Core::Formats::SurfaceBlendMode::Masked ||
                             blend == Core::Formats::SurfaceBlendMode::Opaque )
                            under.push_back( SurfaceDepthEqualVariant() );
                    }
                    if ( pass == MeshPass::Forward )
                        if ( Core::ShaderVariant vsm = ForwardShadowMaskVariant( *program, false, false, true );
                             !vsm.IsDefault() )
                            under.push_back( std::move( vsm ) );
                    for ( Core::ShaderVariant& base : under )
                    {
                        Core::ShaderVariant permutation = SurfaceInstanceIdsVariant( std::move( base ) );
                        if ( listed.emplace( cell.GetValue(), permutation.Hash() ).second )
                            variants.push_back( { cell.GetValue(), std::move( permutation ) } );
                    }
                }
            // The compacted bins' cells through the list's ids (the camera's list, a cascade's own list or the
            // full list's ids): the blade's and, GRASS-GPU-DRIVEN-DRAWS, the mesh grass's (kDrawRowsPaths). A mesh
            // section bins only through ids (DrawBinsOf, BuildGpuInstancedShadowDraws), so its bin cells come from
            // the same id-list materials as its own ids cells above: a grass material has both or neither.
            for ( const MeshVertexPath path : kDrawRowsPaths )
                AppendDrawRowsVariants( materials, shaders, handle, path, /*ids*/ true, listed, variants );
        }
        // CPU-ISM-PACK: the cells an ISM is drawn through on the CPU path by GpuScene id (BuildStaticDraws'
        // instancedCellOf): every material's Instanced Forward / G-buffer / Depth cell in
        // SurfaceSceneInstancesVariant; the G-buffer one also over the equal-depth permutation (an early-Z
        // admitted
        // template, as above), the forward one also over the virtual shadow map's (ForwardVirtualCell).
        for ( const ::Common::AssetHandle& handle : handles )
            for ( const MeshPass pass : { MeshPass::Forward, MeshPass::GBuffer, MeshPass::Depth } )
            {
                const auto cell = materials.CellShaderNameOf( handle, MeshVertexPath::Instanced, pass );
                if ( !cell )
                    continue;
                const auto program = shaders.GetByName( cell.GetValue() );
                if ( program == nullptr )
                    continue;
                std::vector<Core::ShaderVariant> under{ Core::ShaderVariant{} };
                if ( pass == MeshPass::GBuffer )
                {
                    const Core::Formats::SurfaceBlendMode blend = program->GetProgramMeta().Blend;
                    if ( blend == Core::Formats::SurfaceBlendMode::Masked ||
                         blend == Core::Formats::SurfaceBlendMode::Opaque )
                        under.push_back( SurfaceDepthEqualVariant() );
                }
                if ( pass == MeshPass::Forward )
                    if ( Core::ShaderVariant vsm = ForwardShadowMaskVariant( *program, false, false, true );
                         !vsm.IsDefault() )
                        under.push_back( std::move( vsm ) );
                for ( Core::ShaderVariant& base : under )
                {
                    Core::ShaderVariant permutation = SurfaceSceneInstancesVariant( std::move( base ) );
                    if ( listed.emplace( cell.GetValue(), permutation.Hash() ).second )
                        variants.push_back( { cell.GetValue(), std::move( permutation ) } );
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
        // CPU-ISM-PACK 5: the shadow views' ISM casters by GpuScene id (BuildShadowCascadeDraws): the default
        // surface's shared (Instanced x ShadowDepth) caster (SetupShadowPass) and every Masked template's own
        // (MaskedCasterMaterial), each in SurfaceSceneInstancesVariant.
        if ( const auto shared =
                  materials.DefaultSurfaceShader( MeshVertexPath::Instanced, MeshPass::ShadowDepth ) )
        {
            Core::ShaderVariant permutation = SurfaceSceneInstancesVariant();
            if ( shaders.GetByName( shared.GetValue() ) != nullptr &&
                 listed.emplace( shared.GetValue(), permutation.Hash() ).second )
                variants.push_back( { shared.GetValue(), std::move( permutation ) } );
        }
        for ( const ::Common::AssetHandle& handle : handles )
        {
            const auto cell =
                 materials.CellShaderNameOf( handle, MeshVertexPath::Instanced, MeshPass::ShadowDepth );
            if ( !cell )
                continue;
            const auto program = shaders.GetByName( cell.GetValue() );
            if ( program == nullptr || program->GetProgramMeta().Blend != Core::Formats::SurfaceBlendMode::Masked )
                continue; // an opaque template casts through the shared caster above
            Core::ShaderVariant permutation = SurfaceSceneInstancesVariant();
            if ( listed.emplace( cell.GetValue(), permutation.Hash() ).second )
                variants.push_back( { cell.GetValue(), std::move( permutation ) } );
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
        const auto          samples = [&]( const std::shared_ptr<Shader>& shader )
        { return shader && shader->IsCompiled() && shader->SamplesTexture( textureName ); };
        // The queue's columns (m_StaticColumns): a surface is set only for a record with a mesh-cell section, and
        // its translucent flag is IsTranslucent's answer, so an opaque record costs one byte.
        const StaticWalkColumns& columns = m_StaticColumns;
        for ( std::size_t si = 0; si < columns.Count(); ++si )
        {
            if ( columns.Meshes[si] == nullptr || ( columns.Flags[si] & StaticWalkColumns::kTranslucent ) == 0u )
                continue;
            DataDrivenMaterial* const mat = columns.Surfaces[si];
            if ( columns.Instances[si] == nullptr || mat == nullptr )
                continue;
            if ( !StaticEntryInView( frustum, si ) )
                continue;
            // The template and its front layer cell from its facts (ProgramOf): no name asked per section.
            const ResolvedProgram resolved = ProgramOf( *mat );
            if ( samples( resolved.Program ) )
                return true;
            if ( resolved.Facts != nullptr &&
                 mat->GetSchema().Blend == Core::Formats::SurfaceBlendMode::Translucent &&
                 samples( resolved.Facts->FrontLayerProgram ) )
                return true;
        }
        return false;
    }

    bool MeshRenderer::TranslucentSectionsVisible() const
    {
        // DeclareGlassBindings' first three tests (queue entry, frustum, translucent surface): a superset of what
        // it draws (it also drops a refused cell or a missing row), so a false here is a frame the glass node
        // draws nothing in.
        const auto* camera = m_SceneRenderer != nullptr ? m_SceneRenderer->GetMainCamera() : nullptr;
        if ( camera == nullptr )
            return false;
        const Core::Frustum frustum = camera->GetFrustum();
        // The queue's columns (m_StaticColumns): the translucent flag first (one byte), the frustum after it.
        const StaticWalkColumns& columns = m_StaticColumns;
        for ( std::size_t si = 0; si < columns.Count(); ++si )
        {
            if ( columns.Meshes[si] == nullptr || columns.Instances[si] == nullptr ||
                 ( columns.Flags[si] & StaticWalkColumns::kTranslucent ) == 0u )
                continue;
            if ( StaticEntryInView( frustum, si ) )
                return true;
        }
        return false;
    }

    MeshRenderer::LateDepthWriter MeshRenderer::LateDepthWriterInView()
    {
        const auto* camera = m_SceneRenderer != nullptr ? m_SceneRenderer->GetMainCamera() : nullptr;
        if ( camera == nullptr )
            return LateDepthWriter::None;
        const Core::Frustum frustum = camera->GetFrustum();
        // "Deferred: Generic" draws the generic queue's records in the frustum (BuildGenericDraws), and each of
        // their programs writes the depth (blended ones included): one in view is enough.
        for ( std::size_t gi = 0; gi < m_GenericQueue.size(); ++gi )
        {
            if ( m_GenericQueue[gi].Mesh != nullptr && GenericEntryInView( frustum, gi ) )
                return LateDepthWriter::Generic;
        }
        // "Deferred: Glass": DeclareGlassBindings' walk (translucent flag, frustum), and the draw state it draws
        // the section with - its cell's pipeline, the stand-in's while the cell is not ready.
        const auto writesDepth = []( const TranslucentDraw* const draw ) noexcept
        { return draw != nullptr && draw->Pipeline && draw->Pipeline->GetSpecification().DepthWriteEnabled; };
        const TranslucentDraw* const standIn = TranslucentStandIn();
        const StaticWalkColumns&     columns = m_StaticColumns;
        for ( std::size_t si = 0; si < columns.Count(); ++si )
        {
            DataDrivenMaterial* const mat = columns.Surfaces[si];
            if ( columns.Meshes[si] == nullptr || columns.Instances[si] == nullptr || mat == nullptr ||
                 ( columns.Flags[si] & StaticWalkColumns::kTranslucent ) == 0u ||
                 !StaticEntryInView( frustum, si ) )
                continue;
            const TranslucentDraw* const own = TranslucentDrawOf( mat );
            if ( own == nullptr )
                continue; // refused once, by name: the glass draws nothing for it
            // The stand-in too whatever the cell's state: whether the cell is still in the driver this frame is
            // ChoosePipelineDraw's answer, asked (and counted) once, by the glass walk.
            if ( writesDepth( own ) || writesDepth( standIn ) )
                return LateDepthWriter::Translucent;
        }
        return LateDepthWriter::None;
    }

    bool MeshRenderer::QueuedForwardDrawSamples( const std::string_view textureName, const bool blendedOnly ) const
    {
        const auto* camera = m_SceneRenderer != nullptr ? m_SceneRenderer->GetMainCamera() : nullptr;
        if ( camera == nullptr )
            return false;
        const Core::Frustum frustum = camera->GetFrustum();
        for ( std::size_t gi = 0; gi < m_GenericQueue.size(); ++gi )
        {
            const GenericMeshRenderData& g = m_GenericQueue[gi];
            if ( g.Mesh == nullptr || !GenericEntryInView( frustum, gi ) )
                continue;
            // The record's template (GenericProgramOf, resolved once a frame): no name copied or asked per draw.
            const ResolvedProgram&         resolved = GenericProgramOf( gi );
            const std::shared_ptr<Shader>& shader   = resolved.Program;
            if ( !shader || !shader->IsCompiled() )
                continue;
            const bool opaque = resolved.Facts->Opaque;
            if ( ( !blendedOnly || !opaque ) && shader->SamplesTexture( textureName ) )
                return true;
        }
        return false;
    }

    bool MeshRenderer::GenericForwardPipelineReady( ProgramFacts& facts, const std::shared_ptr<Shader>& shader,
                                                    const bool rayTracedSun, const bool localShadowMasks,
                                                    const bool virtualLocal )
    {
        // The forward slice program "Deferred: Generic" shades this draw with (SliceProgramOf: its permutation,
        // key and spec made once per template): the same cache entry, no name built or variant asked per draw.
        SliceProgram* const slice =
             SliceProgramOf( facts, shader, ForwardSliceIndex( rayTracedSun, localShadowMasks, virtualLocal ) );
        if ( slice == nullptr )
            return false; // its refusal was said when the slice program was asked
        const auto&                   built = SlicePipelineOf( *slice ); // the kept answer, no spec hashed
        MaterialPipelineTracker::Row& row   = SliceRowOf( *slice );
        if ( !built )
        {
            MaterialPipelineTracker::OnFailed( row ); // "Deferred: Generic" names the refusal
            return false;
        }
        TrackMaterialPipeline( row, *built.GetValue() );
        return built.GetValue()->GetReadiness() == PipelineReadiness::Ready;
    }

    bool MeshRenderer::OpaqueGenericAdmitted( const std::size_t index ) const
    {
        const ViewFrame* const frame = m_SceneRenderer->GetViewFrame();
        return frame != nullptr && m_GenericPrepassFrame == frame->FrameIndex &&
               index < m_GenericPrepassed.size() && m_GenericPrepassed[index] != 0;
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
        if ( MissInSetupTask( "a two-sided twin pipeline" ) )
            return nullptr;
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

        // Kept across frames (m_ScratchGlassWalk), cleared here.
        TranslucentWalkScratch& walk = m_ScratchGlassWalk;
        walk.Clear();
        auto&                  draws        = walk.Draws;
        auto&                  sortKeys     = walk.SortKeys;
        TranslucentDraw* const standInState = TranslucentStandIn();
        // The queue's columns (m_StaticColumns): the translucent flag (IsTranslucent's answer, one byte) before
        // the frustum, so an opaque record costs this walk no box test and no record read.
        const StaticWalkColumns& columns = m_StaticColumns;
        for ( std::size_t si = 0; si < columns.Count(); ++si )
        {
            MaterialInstance* const   surfaceInst = columns.Instances[si];
            DataDrivenMaterial* const mat         = columns.Surfaces[si];
            if ( columns.Meshes[si] == nullptr || surfaceInst == nullptr ||
                 ( columns.Flags[si] & StaticWalkColumns::kTranslucent ) == 0u )
            {
                continue; // opaque -> drawn by the opaque pass, not here
            }
            if ( !StaticEntryInView( frustum, si ) )
            {
                continue;
            }
            const StaticMeshRenderData& data = m_StaticQueue[si];
            TranslucentDraw* state = TranslucentDrawOf( mat ); // its template's facts: no name asked per frame
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
            const GraphicsPipeline* const glassStandIn =
                 standInState != nullptr ? standInState->Pipeline.get() : nullptr;
            switch ( ChoosePipelineDraw( state->Pipeline.get(), glassStandIn, {} ) )
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
            walk.AddCell( state );
            draws.push_back( { &data, state, *row } );
            const Common::Math::AABB world = StaticEntryWorldBounds( si );
            sortKeys.push_back( { .WorldBoundsCenter = ( world.Min + world.Max ) * 0.5f,
                                  .Priority          = data.TranslucencySortPriority } );
        }
        if ( draws.empty() )
        {
            return;
        }
        // Blending is order-dependent: far glass first, near glass over it (see TranslucentSortOrder.hpp).
        TranslucentSortOrderInto( camera->GetPosition(), sortKeys, walk.SortDistance2, walk.DrawOrder );
        const std::vector<uint32_t>& drawOrder = walk.DrawOrder;

        // --- Per translucent cell, once per frame and BEFORE the blocks are declared, so each cell's route fill
        // is what its draws will read: its rows and the scene snapshot ---
        const SceneFrameBinding frameState = CaptureFrameState( m_SceneRenderer->GetViewFrame() );
        for ( TranslucentDraw* const state : walk.Cells )
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
            const TranslucentObjectDraw& draw = draws[index];
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
                                .LodLevel = ComputeLOD( obj->Transform, obj->Mesh, obj->ForcedLOD, obj->LODBias ),
                                .Push     = DrawPushFields{}
                                             .Transform( glm::mat4( 1.0f ) )
                                             .PrimitiveIndex( obj->GpuPrimitive )
                                             .MaterialIndex( draw.Row ) } );
        }

        // A cell's block also carries the scene snapshot the cells sample for refraction (SceneColorTexture), this
        // frame's graph transient, with the sampler the material route sampled the copy with (linear, REPEAT) -
        // bound only where the cell's layout HAS the slot, as the scene/view inputs are (UE's pass parameters: a
        // shader receives what it declares). The default-surface stand-in (RDG-PSO) samples no scene copy, and
        // naming one to it fails the node ("is not a resource of shader").
        m_GlassDraws.Declare(
             pass, view,
             [sceneCopy, frontLayer]( RDG::BindingBlockBuilder& block, const SlottedLayout& slotted )
             {
                 if ( slotted.Slots.Has( SceneViewSlot::SceneColorTexture ) )
                     block.Texture( MeshForwardShaderParameters::SceneColorTexture, sceneCopy,
                                    RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                          .Sampler( MeshForwardShaderParameters::LinearRepeatSampler,
                                    RDG::SamplerDesc::LinearRepeat() );
                 // The front layer's reflection where this fragment is the front layer
                 // (Pass_Forward_Translucent.glslh), fetched by texel through the state its declaration names.
                 if ( slotted.Slots.Has( SceneViewSlot::FrontLayerReflectionTexture ) )
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
        // Kept across frames (m_ScratchFrontLayerWalk), cleared here.
        TranslucentWalkScratch& walk = m_ScratchFrontLayerWalk;
        walk.Clear();
        auto& draws = walk.Draws;
        // The queue's columns (m_StaticColumns): a front layer is only of a translucent-pass surface (its flag,
        // one byte), so an opaque record costs this walk no box test; Translucent itself is asked of the template.
        const StaticWalkColumns& columns = m_StaticColumns;
        for ( std::size_t si = 0; si < columns.Count(); ++si )
        {
            MaterialInstance* const   surfaceInst = columns.Instances[si];
            DataDrivenMaterial* const mat         = columns.Surfaces[si];
            if ( columns.Meshes[si] == nullptr || surfaceInst == nullptr || mat == nullptr ||
                 ( columns.Flags[si] & StaticWalkColumns::kTranslucent ) == 0u )
                continue;
            if ( mat->GetSchema().Blend != Core::Formats::SurfaceBlendMode::Translucent )
                continue;
            if ( !StaticEntryInView( frustum, si ) )
                continue;
            const StaticMeshRenderData& data  = m_StaticQueue[si];
            TranslucentDraw* const state = FrontLayerDrawOf( mat ); // its template's facts
            if ( state == nullptr )
                continue; // refused once, by name, inside FrontLayerDrawFor
            // A cell still in the driver has no stand-in here: the default surface is opaque and has no front layer
            // cell, so until the cell is ready the object keeps the sky's reflection (said once by
            // ChoosePipelineDraw).
            if ( ChoosePipelineDraw( state->Pipeline.get(), nullptr, {} ) !=
                 MaterialPipelineTracker::CellDraw::Own )
                continue;
            // Its row is the scene material table's (SyncGpuScene: a static draw's receiving row).
            const std::optional<uint32_t> row = FindMaterialRow( mat, surfaceInst );
            if ( !row )
                continue;
            walk.AddCell( state );
            draws.push_back( { &data, state, *row } );
        }
        if ( draws.empty() )
            return false;

        // Per front layer cell, once per frame and before its block is declared: its rows and the scene snapshot
        // (the objects' motion rows its vertex stage and velocity read).
        const SceneFrameBinding frameState = CaptureFrameState( m_SceneRenderer->GetViewFrame() );
        for ( TranslucentDraw* const state : walk.Cells )
        {
            if ( !LendMaterialTable( *state->Material ) )
                return false;
            frameState.ApplyTo( state->Instance.get() );
        }
        for ( const TranslucentObjectDraw& draw : draws )
        {
            const auto*         obj      = draw.Object;
            DataDrivenMaterial* material = draw.State->Material.get();
            MaterialInstance*   instance = draw.State->Instance.get();
            m_FrontLayerDraws.Add(
                 { .Pipeline          = draw.State->Pipeline.get(),
                   .Mesh              = obj->Mesh,
                   .Transform         = glm::mat4( 1.0f ),
                   .Material          = material->GetMaterialExecutor(),
                   .HiddenSubmeshMask = obj->HiddenSubmeshes,
                   .LodLevel          = ComputeLOD( obj->Transform, obj->Mesh, obj->ForcedLOD, obj->LODBias ),
                   .Push              = DrawPushFields{}
                                .Transform( glm::mat4( 1.0f ) )
                                .PrimitiveIndex( obj->GpuPrimitive )
                                .MaterialIndex( draw.Row ) } );
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
    std::size_t MeshRenderer::IsmViewCullKeyHash::operator()( const IsmViewCullKey& key ) const noexcept
    {
        std::size_t h       = std::hash<const void*>{}( key.Transforms );
        const auto  combine = [&h]( const std::size_t v )
        { h ^= v + 0x9E3779B97F4A7C15ull + ( h << 6u ) + ( h >> 2u ); };
        combine( std::hash<const void*>{}( key.Mesh ) );
        combine( std::hash<uint32_t>{}( key.CullMinBits ) );
        combine( std::hash<uint32_t>{}( key.CullMaxBits ) );
        combine( std::hash<uint32_t>{}( key.PadBits ) );
        return h;
    }

    MeshRenderer::IsmViewCullLookup MeshRenderer::LookUpIsmViewCull( const InstancedMeshRenderData& ism,
                                                                     const Core::Frustum&           frustum,
                                                                     const Geometry::LODView&       view )
    {
        const Common::Math::AABB localBounds = Geometry::LocalBounds( ism.Mesh->GetSubmeshes() );
        std::array<glm::vec4, 6> planes{};
        for ( std::size_t p = 0; p < planes.size(); ++p )
        {
            const Core::Frustum::Plane& plane = frustum.GetPlane( static_cast<Core::FrustumPlane>( p ) );
            planes[p]                         = glm::vec4( plane.Normal, plane.Distance );
        }
        const IsmViewCullKey      key{ .Transforms  = ism.Transforms.get(),
                                       .Mesh        = ism.Mesh,
                                       .CullMinBits = std::bit_cast<uint32_t>( ism.CullDistance.Min ),
                                       .CullMaxBits = std::bit_cast<uint32_t>( ism.CullDistance.Max ),
                                       .PadBits     = std::bit_cast<uint32_t>( WindBoundsPad( ism.Wind ) ) };
        const std::vector<float>& lodSizes = ism.Mesh->GetLODScreenSizes();
        IsmViewCull&              entry    = m_IsmViewCulls[key];
        entry.Generation                   = m_IsmViewCullGeneration;
        // A second ISM of the key in this call (the field's other sections): the same inputs, one cull.
        if ( entry.Call == m_IsmViewCullCall )
            return { .Entry = &entry, .Stale = false };
        entry.Call = m_IsmViewCullCall;
        const bool same = entry.Snapshot == ism.Transforms && entry.LocalBounds.Min == localBounds.Min &&
                          entry.LocalBounds.Max == localBounds.Max && entry.Planes == planes &&
                          entry.View.Origin == view.Origin && entry.View.Projection == view.Projection &&
                          entry.LodScreenSizes == lodSizes;
        if ( same )
            return { .Entry = &entry, .Stale = false };
        entry.Snapshot       = ism.Transforms;
        entry.LocalBounds    = localBounds;
        entry.Planes         = planes;
        entry.View           = view;
        entry.LodScreenSizes = lodSizes;
        ++entry.Rebuilds;
        return { .Entry = &entry, .Stale = true };
    }

    void MeshRenderer::RunStaleIsmViewCulls( const std::span<const DrawBuildScratch::StaleIsmCull> stale,
                                             const Core::Frustum&                                  frustum )
    {
        if ( stale.empty() )
            return;
        DESERT_PROFILE_SCOPE( "Mesh: ISM view culls" );
        std::size_t total = 0;
        for ( const DrawBuildScratch::StaleIsmCull& item : stale )
            total += item.Ism->Transforms->size();
        const std::size_t   participants = ::Common::JobSystem::Get().WorkerCount() + 1;
        const std::size_t   slices       = std::min( stale.size(), participants );
        ::Common::TaskGroup tasks( m_SceneRenderer->ParallelSetup() );
        std::size_t         first   = 0;
        std::size_t         reached = 0; // the instances of stale[0, first)
        for ( std::size_t s = 0; s < slices && first < stale.size(); ++s )
        {
            // Slice s ends where the instances reach total * (s + 1) / slices; the last one at the end.
            const bool        lastSlice = s + 1 == slices;
            const std::size_t target    = total * ( s + 1 ) / slices;
            std::size_t       last      = first + 1;
            reached += stale[first].Ism->Transforms->size();
            while ( last < stale.size() && ( lastSlice || reached < target ) )
                reached += stale[last++].Ism->Transforms->size();
            tasks.Launch( "Mesh: ISM view cull",
                          [stale, &frustum, first, last]() noexcept
                          {
                              for ( std::size_t i = first; i < last; ++i )
                                  CollectIsmViewCull( *stale[i].Entry, *stale[i].Ism, frustum );
                          } );
            first = last;
        }
        tasks.Join();
    }

    void MeshRenderer::PrepareViewIsmIds()
    {
        m_ViewIsmIdsReady = false;
        m_ViewIsmIdRanges.assign( m_InstancedQueue.size(), ViewIsmIdRange{} );
        const auto* const camera = m_SceneRenderer->GetMainCamera();
        if ( !m_FrameSceneInstances.IsValid() || camera == nullptr || m_InstancedQueue.empty() )
            return;
        DESERT_PROFILE_SCOPE( "Mesh: ISM view ids" );
        // The main view's cull, as BuildStaticDraws asks it (the same frustum and LOD view): the passes' look-ups
        // then find every entry current and cull nothing again this frame.
        const Core::Frustum     frustum = camera->GetFrustum();
        const Geometry::LODView mainView{ camera->GetPosition(), camera->GetProjectionMatrix() };
        ++m_IsmViewCullCall;
        m_ViewIsmStale.clear();
        for ( std::size_t f = 0; f < m_InstancedQueue.size(); ++f )
        {
            const InstancedMeshRenderData& ism = m_InstancedQueue[f];
            if ( ism.Mesh == nullptr || !ism.Material || !ism.Transforms || ism.Transforms->empty() )
                continue;
            const IsmViewCullLookup cull = LookUpIsmViewCull( ism, frustum, mainView );
            m_ViewIsmIdRanges[f].Entry = cull.Entry;
            if ( cull.Stale )
                m_ViewIsmStale.push_back( { .Entry = cull.Entry, .Ism = &ism } );
        }
        RunStaleIsmViewCulls( m_ViewIsmStale, frustum );

        // Each ISM its own range (two sections of a field share a cull entry but not their GpuScene run): its
        // kept instances in the entry's level order, so a pass's draw of level run r is Base + r.First.
        m_ViewIsmIdWords.clear();
        for ( std::size_t f = 0; f < m_InstancedQueue.size(); ++f )
        {
            const IsmViewCull* const entry = m_ViewIsmIdRanges[f].Entry;
            if ( entry == nullptr )
                continue;
            const uint32_t gpuFirst = m_InstancedQueue[f].FirstGpuInstance;
            m_ViewIsmIdRanges[f]     = { .Base     = static_cast<uint32_t>( m_ViewIsmIdWords.size() ),
                                         .Rebuilds = entry->Rebuilds,
                                         .Entry    = entry };
            for ( const uint32_t index : entry->Indices )
                m_ViewIsmIdWords.push_back( gpuFirst + index );
        }
        if ( m_ViewIsmIdWords.empty() )
            m_ViewIsmIdWords.push_back( 0u ); // a buffer is never empty; no draw names the word

        const auto bytes = static_cast<uint32_t>( m_ViewIsmIdWords.size() * sizeof( uint32_t ) );
        if ( m_ViewIsmIds == nullptr )
        {
            // Bound at each lending property's own InstanceTransforms slot (StorageBufferProperty::GetBinding).
            m_ViewIsmIds = ShaderResources::StorageBuffer::Create( "MeshRenderer.ViewIsmIds", bytes, 0u );
            if ( m_ViewIsmIds == nullptr )
            {
                LOG_ERROR( "[MeshRenderer] the view's ISM id buffer ({} bytes) could not be created; the ISMs "
                           "pack their transforms",
                           bytes );
                return;
            }
        }
        const auto wrote = m_ViewIsmIds->SetData( m_ViewIsmIdWords.data(), bytes );
        if ( !wrote.IsSuccess() )
        {
            LOG_ERROR( "[MeshRenderer] the view's ISM ids ({} bytes) were not uploaded; the ISMs pack their "
                       "transforms -- {}",
                       bytes, wrote.GetError() );
            return;
        }
        m_ViewIsmIdsReady = true;
    }

    void MeshRenderer::CollectIsmViewCull( IsmViewCull& entry, const InstancedMeshRenderData& ism,
                                           const Core::Frustum& frustum )
    {
        DESERT_PROFILE_SCOPE( "Mesh: ISM view cull (rebuilt)" );
        CollectIsmInstances( *ism.Transforms, entry.LocalBounds, frustum, ism.CullDistance, ism.Wind, entry.View,
                             entry.View, entry.LodScreenSizes, entry.Visible, entry.Levels, &entry.Indices );
        OrderIsmViewCullByLevel( entry );
    }

    void MeshRenderer::OrderIsmViewCullByLevel( IsmViewCull& entry ) noexcept
    {
        // The distinct levels, ascending (Geometry::DistinctLODs' order), with their counts.
        std::vector<IsmLevelRun>& runs = entry.Runs;
        runs.clear();
        for ( const uint32_t level : entry.Levels )
        {
            const auto found = std::find_if( runs.begin(), runs.end(),
                                             [level]( const IsmLevelRun& run ) { return run.Level == level; } );
            if ( found != runs.end() )
                ++found->Count;
            else
                runs.push_back( { .Level = level, .First = 0, .Count = 1 } );
        }
        std::sort( runs.begin(), runs.end(),
                   []( const IsmLevelRun& a, const IsmLevelRun& b ) { return a.Level < b.Level; } );
        uint32_t first = 0;
        for ( IsmLevelRun& run : runs )
        {
            run.First = first;
            first += run.Count;
        }
        if ( runs.size() <= 1 )
            return; // one level (or none): Visible is its run already
        // The stable partition: each kept instance at its level's next slot, in kept order (Count is the cursor,
        // back to the count when every instance is placed).
        for ( IsmLevelRun& run : runs )
            run.Count = 0;
        // The indices move with their transforms (an entry the cull filled without them has none).
        const bool indexed = entry.Indices.size() == entry.Visible.size();
        entry.Ordered.resize( entry.Visible.size() );
        entry.OrderedIndices.resize( indexed ? entry.Indices.size() : 0u );
        for ( std::size_t i = 0; i < entry.Visible.size(); ++i )
        {
            const uint32_t level = entry.Levels[i];
            for ( IsmLevelRun& run : runs )
                if ( run.Level == level )
                {
                    const uint32_t slot = run.First + run.Count++;
                    entry.Ordered[slot] = entry.Visible[i];
                    if ( indexed )
                        entry.OrderedIndices[slot] = entry.Indices[i];
                    break;
                }
        }
        entry.Visible.swap( entry.Ordered );
        if ( indexed )
            entry.Indices.swap( entry.OrderedIndices );
        for ( const IsmLevelRun& run : runs )
            std::fill_n( entry.Levels.begin() + run.First, run.Count, run.Level );
    }

    void MeshRenderer::WalkStaticChunk( const ClusterWalkView& view, const Core::Frustum& frustum,
                                        const std::size_t first, const std::size_t last,
                                        DrawBuildScratch::StaticWalkChunk& chunk ) const
    {
        chunk.GroupCount = 0;
        chunk.Masks.clear();
        chunk.Words.clear();
        // The chunk's own groupFor: its groups reused by index, so their vectors keep their capacity.
        const auto groupOf = [&chunk]( DataDrivenMaterial* mat ) -> std::vector<const StaticMeshRenderData*>&
        {
            for ( std::size_t g = 0; g < chunk.GroupCount; ++g )
                if ( chunk.Groups[g].first == mat )
                    return chunk.Groups[g].second;
            if ( chunk.GroupCount == chunk.Groups.size() )
                chunk.Groups.emplace_back();
            DrawBuildScratch::StaticGroup& made = chunk.Groups[chunk.GroupCount++];
            made.first                          = mat;
            made.second.clear();
            return made.second;
        };
        // The queue's hot columns (m_StaticColumns, dense by position): the record itself is touched only when it
        // joins a group or a mask (its address). A record past the columns was queued after the sync: skipped.
        const StaticWalkColumns& columns = m_StaticColumns;
        const std::size_t        end     = std::min( last, columns.Count() );
        // The snapshot's boxes four at a time (CPU-SIMD 4; UE's primitive-bounds batch): the group of a record
        // that reaches the frustum test is tested once over the snapshot's lanes, kept with the static revision;
        // each record reads its bit - StaticEntryInView's answer. The chunk's own cursor: chunks run in parallel.
        const bool lanes      = StaticLanesCurrent();
        const auto entryGroup = [this, &frustum]( const std::size_t base ) noexcept
        { return StaticEntriesInView4( frustum, base ); };
        VisibleInViewQuads entryQuads;
        for ( std::size_t i = first; i < end; ++i )
        {
            const Desert::Mesh* const mesh = columns.Meshes[i];
            if ( mesh == nullptr || ( columns.Flags[i] & StaticWalkColumns::kHasSlots ) == 0u )
                continue;
            const uint64_t hidden = columns.Hidden[i];

            // Held by the view's GPU-culled bins this frame (AttachBinned, CpuWalkHidden): the cull decides it and
            // the bins issue it - asked before the CPU frustum test, so a binned record costs this loop no culling
            // work.
            const std::optional<UninternedWalk> walk =
                 CpuWalkUninterned( view, mesh, columns.Primitives[i], hidden, chunk.WalkWords );
            if ( !walk )
                continue;
            // The dense snapshot's box (m_StaticSnapshot, built once per static revision), not this entry's mesh.
            const bool inView = lanes ? entryQuads.Visible( i, entryGroup ) : StaticEntryInView( frustum, i );
            if ( !inView )
                continue;
            const StaticMeshRenderData& data = m_StaticQueue[i];
            // Words past submesh 63 make a mask unlike the entry's own (an interned view never equals none).
            if ( walk->HighWords != 0u || walk->Low != hidden )
            {
                chunk.Masks.push_back( { .Data      = &data,
                                         .Low       = walk->Low,
                                         .WordsAt   = static_cast<uint32_t>( chunk.Words.size() ),
                                         .HighWords = static_cast<uint32_t>( walk->HighWords ) } );
                const auto high = chunk.WalkWords.begin() + 1;
                chunk.Words.insert( chunk.Words.end(), high,
                                    high + static_cast<std::ptrdiff_t>( walk->HighWords ) );
            }

            // The entry's section slot drives its batch (one entry per section material, made at submit by
            // Rules::SplitMeshSlotDraws; custom-shader sections went to the generic path and are masked out). Its
            // surface column is SectionSurface's answer, asked once at the sync.
            if ( columns.Instances[i] != nullptr )
                groupOf( columns.Surfaces[i] ).push_back( &data );
        }
    }

    MeshRenderer::StaticCellDrawCache& MeshRenderer::StaticCellDrawsOf( const MeshPass            pass,
                                                                        const DeferredPrepassHalf half,
                                                                        const uint64_t            key ) noexcept
    {
        ++m_StaticCellDrawUses;
        StaticCellDrawCache* slot = nullptr;
        for ( StaticCellDrawCache& cache : m_StaticCellDrawCaches )
        {
            if ( cache.InUse && cache.Pass == pass && cache.Half == half )
            {
                slot = &cache;
                break;
            }
        }
        if ( slot == nullptr )
        {
            // An unused slot, else the least recently used one (six cover every (pass, half) a frame builds).
            for ( StaticCellDrawCache& cache : m_StaticCellDrawCaches )
            {
                if ( !cache.InUse )
                {
                    slot = &cache;
                    break;
                }
                if ( slot == nullptr || cache.UsedAt < slot->UsedAt )
                    slot = &cache;
            }
            slot->Pass  = pass;
            slot->Half  = half;
            slot->InUse = true;
            slot->Entries.clear(); // another (pass, half)'s answers: capacity kept
            slot->Key = key;
        }
        else if ( slot->Key != key )
        {
            slot->Entries.clear(); // an input moved: every answer is asked again, once
            slot->Key = key;
        }
        slot->UsedAt = m_StaticCellDrawUses;
        return *slot;
    }

    uint32_t MeshRenderer::BuildStaticDraws( MeshDrawList& list, const DrawBuildMode& mode,
                                             DrawBuildScratch& scratch )
    {
        uint32_t ismInstances = 0; // the ISM instances drawn (returned; a Depth half counts none)
        // DrawBuildPhase: a Record call takes what its half's Resolve call planned into the scratch, and only it.
        const bool resolveCall = mode.Phase == DrawBuildPhase::Resolve;
        const bool recordCall  = mode.Phase == DrawBuildPhase::Record;
        if ( recordCall && !scratch.Planned )
            return 0;
        if ( !recordCall )
        {
            scratch.Planned      = false;
            scratch.InstSetCount = 0;
            scratch.RecycleGroups( scratch.Groups ); // their lists keep their capacity (SpareLists)
            scratch.Walked.Reset( m_StaticQueue.size() );
            if ( mode.Walk == nullptr )
                scratch.RecycleGroups( scratch.WalkGroups );
        }
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
            return 0;
        // The Wireframe view's static and instanced twins write their own depth: not prepassed.
        if ( mode.Pass == MeshPass::Depth && WireframeView() )
            return 0;

        auto* const camera = m_SceneRenderer->GetMainCamera();
        if ( camera == nullptr )
            return 0;

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
        auto&      groups   = scratch.Groups;
        const auto groupFor = [&]( DataDrivenMaterial* mat ) -> std::vector<const StaticMeshRenderData*>&
        {
            for ( auto& [m, v] : groups )
                if ( m == mat )
                    return v;
            groups.emplace_back( mat, scratch.TakeList() );
            return groups.back().second;
        };

        // THE CPU WALK of the main view (CpuWalkHidden, the one rule of every view pass): a record its bins hold
        // is issued from them; a VG primitive (out of the bins, the cluster raster's) is walked for every submesh
        // the raster does not draw - a refused cut / carrier or a twin-less group leaves it to its own submeshes.
        // The masks that differ from an entry's own HiddenSubmeshes (a partly clustered VG entry) are kept here.
        // The forward depth prepass walks every record (no bins are attached to it): the base pass's binned draws
        // test against the depth it wrote. The deferred early-Z pass walks the G-buffer's CPU records alone: a
        // record the G-buffer bins hold is drawn by its bin, with its own depth.
        const ClusterWalkView mainWalk =
             mode.Pass == MeshPass::Depth && mode.Half != DeferredPrepassHalf::Depth
                  ? UnbinnedWalkView( GpuDriven::MeshPass::DepthPrepass )
                  : BinnedWalkView( mode.Pass == MeshPass::Forward ? GpuDriven::MeshPass::Forward
                                                                        : GpuDriven::MeshPass::GBuffer,
                                    kBinnedMainView );
        // Dense by m_StaticQueue position (DrawBuildScratch::WalkedMasks): an index, never a hash per object.
        DrawBuildScratch::WalkedMasks& walked = scratch.Walked;
        const StaticMeshRenderData* const queueBase = m_StaticQueue.data();
        const auto hiddenOf = [&walked, queueBase]( const StaticMeshRenderData* obj ) -> SubmeshMask
        {
            const auto     at   = static_cast<std::size_t>( obj - queueBase );
            const uint32_t slot = at < walked.At.size() ? walked.At[at] : 0u;
            if ( slot == 0u )
                return SubmeshMask{ obj->HiddenSubmeshes };
            return walked.Masks[slot - 1u];
        };
        // The CPU walk of the queue, timed as one row (GPU-COST-37: Add passes' split). ONE walk for both G-buffer
        // halves: the Rest half's Resolve call takes the Equal half's (DrawBuildMode::Walk), and a Record call
        // walks nothing (its groups are its Resolve call's).
        if ( resolveCall && mode.Walk != nullptr )
        {
            scratch.CopyGroups( groups, mode.Walk->WalkGroups );
            walked = mode.Walk->Walked;
        }
        else if ( !recordCall )
        {
            DESERT_PROFILE_SCOPE( "Mesh: Static walk" );
            // ON THE WORKERS (RENDER-PARALLEL-SETUP; UE FParallelMeshDrawCommandPass / ComputeViewVisibility: the
            // walk is a ParallelFor over contiguous chunks, each into its own list, merged in primitive order).
            // About WorkerCount() + 1 chunks of the queue - the joining thread walks one too - none below
            // kStaticWalkChunkMin records; each is one setup task writing only its own
            // DrawBuildScratch::WalkChunks entry (WalkStaticChunk). The merge is serial and in queue order: per
            // chunk, its masks interned in queue order (the walk's one write, InternWalkWords), then its groups in
            // first-seen order appended through groupFor. A material's first record lies in the first chunk that
            // holds it, at its first place there, so the groups come out in the serial walk's first-seen order,
            // and each group's records in queue order: the same groups, records and masks as one walk of the whole
            // queue. ParallelSetup 0 walks the chunks inline, in queue order.
            constexpr std::size_t kStaticWalkChunkMin = 128; // records: below it a task costs more than it walks
            const std::size_t     queued              = m_StaticQueue.size();
            const std::size_t     participants        = ::Common::JobSystem::Get().WorkerCount() + 1;
            const std::size_t chunks = std::clamp<std::size_t>( queued / kStaticWalkChunkMin, 1u, participants );
            if ( scratch.WalkChunks.size() < chunks )
                scratch.WalkChunks.resize( chunks );
            {
                ::Common::TaskGroup tasks( m_SceneRenderer->ParallelSetup() );
                for ( std::size_t c = 0; c < chunks; ++c )
                {
                    const std::size_t                  first = queued * c / chunks;
                    const std::size_t                  last  = queued * ( c + 1 ) / chunks;
                    DrawBuildScratch::StaticWalkChunk& chunk = scratch.WalkChunks[c];
                    tasks.Launch( "Mesh: Static walk chunk",
                                  [this, &mainWalk, &frustum, &chunk, first, last]() noexcept
                                  { WalkStaticChunk( mainWalk, frustum, first, last, chunk ); } );
                }
                tasks.Join();
            }
            // A mask that cannot be interned (a walk inside a setup task, MissInSetupTask) drops its record, as
            // the serial walk's CpuWalkHidden did: none in a Full or a Resolve call.
            std::vector<const StaticMeshRenderData*>& unwalked = scratch.Unwalked;
            unwalked.clear();
            for ( std::size_t c = 0; c < chunks; ++c )
            {
                const DrawBuildScratch::StaticWalkChunk& chunk = scratch.WalkChunks[c];
                for ( const DrawBuildScratch::WalkedMask& masked : chunk.Masks )
                {
                    std::optional<SubmeshMask> mask = SubmeshMask{ masked.Low };
                    if ( masked.HighWords != 0u )
                        mask = InternWalkWords( masked.Low, std::span<const uint64_t>( chunk.Words )
                                                                 .subspan( masked.WordsAt, masked.HighWords ) );
                    if ( !mask )
                        unwalked.push_back( masked.Data );
                    else if ( !( *mask == SubmeshMask{ masked.Data->HiddenSubmeshes } ) )
                        walked.Set( static_cast<std::size_t>( masked.Data - queueBase ), *mask );
                }
                for ( std::size_t g = 0; g < chunk.GroupCount; ++g )
                {
                    const auto& [mat, objects] = chunk.Groups[g];
                    if ( unwalked.empty() )
                    {
                        auto& into = groupFor( mat );
                        into.insert( into.end(), objects.begin(), objects.end() );
                        continue;
                    }
                    for ( const StaticMeshRenderData* obj : objects )
                        if ( std::find( unwalked.begin(), unwalked.end(), obj ) == unwalked.end() )
                            groupFor( mat ).push_back( obj );
                }
            }
            if ( resolveCall )
                scratch.CopyGroups( scratch.WalkGroups, groups ); // before the split: the other Resolve takes it
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
        // The deferred early-Z pairing (DeferredPrepassHalf): the prepass's companion is the G-buffer's DepthEqual
        // cells, the Equal half's is the prepass's; the Rest half pairs with nothing.
        const StaticPassCells cells = StaticCellsOf( mode.Pass, mode.Half == DeferredPrepassHalf::Equal );
        std::optional<StaticPassCells> companion;
        if ( mode.Half == DeferredPrepassHalf::Depth )
            companion = StaticCellsOf( MeshPass::GBuffer, /*depthEqual*/ true );
        else if ( mode.Half == DeferredPrepassHalf::Equal )
            companion = StaticCellsOf( MeshPass::Depth );
        else if ( mode.Half == DeferredPrepassHalf::None && PairsWithPrepass( mode.Pass, /*wireframe*/ true ) )
            companion = StaticCellsOf( mode.Pass == MeshPass::Depth ? MeshPass::Forward : MeshPass::Depth );
        GraphicsPipeline* const   instancedPipeline = cells.InstancedState;
        DataDrivenMaterial* const instancedMaterial = cells.InstancedSpare;
        MaterialInstance* const   instancedInstance = cells.InstancedInstance;
        const bool     instancingOn  = cells.InstancingOn && ( !companion || companion->InstancingOn );
        const MeshPass instancedPass = mode.Pass;
        auto* const    materialService = Runtime::ResourceRegistry::GetMaterialService();
        // A `.demat`'s (Instanced x pass) and per-object (Static x pass) cells: the Forward ones through their
        // virtual-shadow permutation (ForwardVirtualCell), the others the service's sibling.
        // After the deferred early-Z pass (c.DepthEqual): the G-buffer's DepthEqualCell.
        // @p sceneIds: an ISM's cell by GpuScene id (SurfaceSceneInstancesVariant, over the same permutations).
        const auto instancedCellOf = [&]( const StaticPassCells& c, DataDrivenMaterial* mat,
                                          const bool sceneIds ) -> DataDrivenMaterial*
        {
            if ( !sceneIds )
            {
                if ( c.DepthEqual )
                    return DepthEqualCell( mat, MeshVertexPath::Instanced );
                return ForwardVirtualCell( materialService->GetVariant( mat, MeshVertexPath::Instanced, c.Pass ),
                                           mat, MeshVertexPath::Instanced, c.Pass );
            }
            // The service's permutation lookup interns the sibling (a write): asked serially (the ISM loop's
            // first step), never on a setup task.
            if ( materialService == nullptr || MissInSetupTask( "an ISM's GpuScene-id cell" ) )
                return nullptr;
            if ( c.DepthEqual )
                return materialService->GetVariant( mat, MeshVertexPath::Instanced, MeshPass::GBuffer,
                                                    SurfaceSceneInstancesVariant( SurfaceDepthEqualVariant() ) );
            DataDrivenMaterial* const cell =
                 materialService->GetVariant( mat, MeshVertexPath::Instanced, c.Pass,
                                              SurfaceSceneInstancesVariant() );
            return ForwardVirtualCell( cell, mat, MeshVertexPath::Instanced, c.Pass, std::nullopt, false, true );
        };
        const auto perObjectCellOf = [&]( const StaticPassCells& c, DataDrivenMaterial* mat ) -> DataDrivenMaterial*
        {
            if ( c.DepthEqual )
                return DepthEqualCell( mat, MeshVertexPath::Static );
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
        auto& instSets = scratch.InstSets; // InstSetCount reset above (a Record call keeps its Resolve's sets)
        // @p sceneIds: the recorder is an ISM's cell by GpuScene id (its instances: view id ranges).
        const auto setFor = [&]( DataDrivenMaterial* recorder, MaterialInstance* inst,
                                 const bool sceneIds ) -> InstancedBatchSet&
        {
            for ( std::size_t i = 0; i < scratch.InstSetCount; ++i )
                if ( instSets[i]->Mat == recorder && instSets[i]->SceneIds == sceneIds )
                    return *instSets[i];
            if ( scratch.InstSetCount == instSets.size() )
                instSets.push_back( std::make_unique<InstancedBatchSet>() );
            InstancedBatchSet& set = *instSets[scratch.InstSetCount++];
            set.Mat                = recorder;
            set.Inst               = inst;
            set.Transforms.clear();
            set.SceneIds = sceneIds;
            set.Draws.clear();
            set.IsmDraws.clear();
            set.OneSided = nullptr;
            set.TwoSided = nullptr;
            set.Lent     = false;
            set.Drawn    = false;
            return set;
        };

        // The accumulator a group's batches belong to, or null when this group must not be batched at
        // all. The DECISION is SelectInstancedRecorder — two booleans, so that the branch that was wrong
        // is testable without a Vulkan device; this lambda is only the wiring from the answer to a
        // material, an instance and a bucket.
        // @p sceneIds: an ISM drawn by GpuScene id (its cells in SurfaceSceneInstancesVariant); false for a group.
        const auto setForGroup = [&]( DataDrivenMaterial* group, const bool sceneIds ) -> InstancedBatchSet*
        {
            if ( !instancingOn || group == nullptr )
                return nullptr;

            auto*               materials = Runtime::ResourceRegistry::GetMaterialService();
            const bool          hasAsset  = materials != nullptr && materials->Owns( group );
            DataDrivenMaterial* variant = hasAsset ? instancedCellOf( cells, group, sceneIds ) : nullptr;
            // Paired with the prepass: batched only when BOTH passes have the cell, so both batch alike.
            if ( variant != nullptr && companion && instancedCellOf( *companion, group, sceneIds ) == nullptr )
                variant = nullptr;

            switch ( SelectInstancedRecorder( hasAsset, variant != nullptr ) )
            {
                case InstancedRecorder::RendererSpare:
                    return &setFor( instancedMaterial, instancedInstance, false );

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
                    return &setFor( variant, cached.get(), sceneIds );
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
                if ( DataDrivenMaterial* const variant = instancedCellOf( c, mat, /*sceneIds*/ false ) )
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
            if ( mode.Pass == MeshPass::Forward || mode.Half == DeferredPrepassHalf::Equal )
                if ( const auto decided = PrepassCellDraw( key ) )
                    return *decided;
            const CellDraw draw =
                 worse( cellDrawIn( cells, mat, twoSided, perObject ), cellDrawIn( *companion, mat, twoSided, perObject ) );
            if ( mode.Pass == MeshPass::Depth )
                RecordPrepassCellDraw( key, draw );
            return draw;
        };
        // AN ISM's CELL, the PSO precache's rule (UE: a draw waits for ITS pipeline and draws as before until it
        // is ready): by GpuScene id once the scene-id cell (SurfaceSceneInstancesVariant) is Ready in this pass
        // and in its companion and this graph has the scene's instance rows; until then its packed transforms
        // through the plain instanced cell (cellDrawOf, perObject false) - the ISM is drawn on every frame of the
        // switch. Paired with the prepass both answers are the prepass's record (the plain key holds the ISM's
        // effective draw, so DeferredHalfTakes splits it by what is actually drawn).
        struct IsmCellChoice
        {
            CellDraw Draw     = CellDraw::Nothing;
            bool     SceneIds = false;
        };
        const auto sceneIdCellReadyIn = [&]( const StaticPassCells& c, DataDrivenMaterial* mat,
                                             const bool twoSided ) -> bool
        {
            if ( !instancingOn || materialService == nullptr || !materialService->Owns( mat ) )
                return false; // a renderer-owned material is recorded by the spare (packed transforms)
            DataDrivenMaterial* const cell = instancedCellOf( c, mat, /*sceneIds*/ true );
            return cell != nullptr &&
                   ChooseCellDraw( c.InstancedState, *cell, twoSided, c.InstancedStandIn ) == CellDraw::Own;
        };
        const auto ismCellOf = [&]( DataDrivenMaterial* mat, const bool twoSided ) -> IsmCellChoice
        {
            // The scene's instance rows in this graph and the view's ids uploaded (PrepareViewIsmIds).
            const bool sceneRows = m_FrameSceneInstances.IsValid() && m_ViewIsmIdsReady;
            if ( !companion )
            {
                if ( sceneRows && sceneIdCellReadyIn( cells, mat, twoSided ) )
                    return { .Draw = CellDraw::Own, .SceneIds = true };
                return { .Draw = cellDrawIn( cells, mat, twoSided, /*perObject*/ false ), .SceneIds = false };
            }
            const PrepassCellKey key{ .Material = mat, .Path = MeshVertexPath::Static, .TwoSided = twoSided };
            const PrepassCellKey idKey{
                 .Material = mat, .Path = MeshVertexPath::Static, .TwoSided = twoSided, .SceneInstances = true };
            if ( mode.Pass == MeshPass::Forward || mode.Half == DeferredPrepassHalf::Equal )
                if ( const auto decided = PrepassCellDraw( key ) )
                    return { .Draw     = *decided,
                             .SceneIds = sceneRows && PrepassCellDraw( idKey ) == CellDraw::Own };
            const bool ids = sceneRows && sceneIdCellReadyIn( cells, mat, twoSided ) &&
                             sceneIdCellReadyIn( *companion, mat, twoSided );
            CellDraw draw = CellDraw::Own;
            if ( !ids )
                draw = worse( cellDrawIn( cells, mat, twoSided, false ),
                              cellDrawIn( *companion, mat, twoSided, false ) );
            if ( mode.Pass == MeshPass::Depth )
            {
                RecordPrepassCellDraw( key, draw );
                RecordPrepassCellDraw( idKey, ids ? CellDraw::Own : CellDraw::Nothing );
            }
            return { .Draw = draw, .SceneIds = ids };
        };
        // A group is two-sided when its surface is or ANY QUEUED member's instance is (UE: per mesh batch, not per
        // visible set): the plan rows' answer (StaticPlanRows::SurfaceTwoSided), fixed while their key stands, so
        // the group's culled permutation does not flip with what the camera sees. A surface the rows do not know
        // (no rows yet) scans the group's members as before.
        const auto groupTwoSided =
             [this]( DataDrivenMaterial* mat, const std::vector<const StaticMeshRenderData*>& members )
        {
            bool twoSided = mat->IsTwoSided();
            if ( const std::optional<bool> queued = QueuedSurfaceTwoSided( mat ) )
                return twoSided || *queued;
            for ( const auto* obj : members )
                if ( const auto* inst = SectionSurface( obj->MaterialSlots.get(), obj->SectionSlot ).Instance )
                    twoSided = twoSided || inst->IsTwoSided();
            return twoSided;
        };
        // A GROUP's CELL ANSWER, RETAINED (StaticCellDrawCache; UE FCachedPassMeshDrawList): asked of the cells
        // only when its key moved - the draw readiness epoch (DrawReadinessKey, read here, before this call's
        // first answer) and every input cellDrawOf reads besides the material and its two-sidedness. A pass that
        // takes the prepass's record (the forward base pass, the G-buffer's Equal half) reads that record as
        // before; a renderer-owned material is asked every time (its forward VSM twin is keyed by address).
        const bool takesPrepassRecord =
             companion && ( mode.Pass == MeshPass::Forward || mode.Half == DeferredPrepassHalf::Equal );
        StaticCellDrawCache* kept = nullptr;
        // Never from a setup task: ChooseCellDraw answers Nothing there (MissInSetupTask), not an answer to keep.
        if ( !recordCall && !takesPrepassRecord && materialService != nullptr && !::Common::TaskGroup::InTask() )
        {
            uint64_t   key = DrawReadinessKey();
            const auto mix = [&key]( const uint64_t value ) noexcept { key = ( key ^ value ) * 0x100000001B3ull; };
            const auto bits = []( const void* p ) noexcept
            { return static_cast<uint64_t>( reinterpret_cast<std::uintptr_t>( p ) ); };
            const auto mixCells = [&]( const StaticPassCells& c )
            {
                mix( static_cast<uint64_t>( c.Pass ) );
                mix( bits( c.StaticState ) );
                mix( bits( c.StandInPipeline ) );
                mix( bits( c.InstancedState ) );
                mix( bits( c.InstancedStandIn ) );
                mix( ( c.InstancingOn ? 1u : 0u ) | ( c.DepthEqual ? 2u : 0u ) );
            };
            mixCells( cells );
            mix( companion ? 1u : 0u );
            if ( companion )
                mixCells( *companion );
            mix( ( instancingOn ? 1u : 0u ) | ( m_ForwardVirtualLocal ? 2u : 0u ) );
            mix( bits( materialService ) );
            kept = &StaticCellDrawsOf( mode.Pass, mode.Half, key );
        }
        const auto keptCellDrawOf = [&]( DataDrivenMaterial* mat, const bool twoSided ) -> CellDraw
        {
            if ( kept == nullptr || mat == nullptr || !materialService->Owns( mat ) )
                return cellDrawOf( mat, twoSided, /*perObject*/ true );
            std::vector<StaticCellDrawEntry>&                entries = kept->Entries;
            const std::pair<const DataDrivenMaterial*, bool> wanted{ mat, twoSided };
            const auto at = std::lower_bound( entries.begin(), entries.end(), wanted,
                                              []( const StaticCellDrawEntry&                        entry,
                                                  const std::pair<const DataDrivenMaterial*, bool>& k ) noexcept
                                              {
                                                  if ( entry.Material != k.first )
                                                      return std::less<const DataDrivenMaterial*>{}(
                                                           entry.Material, k.first );
                                                  return entry.TwoSided < k.second;
                                              } );
            if ( at != entries.end() && at->Material == mat && at->TwoSided == twoSided )
            {
                m_StandInDraws += at->StandIns;
                // The prepass's record is per frame (cleared at its Declare): written again from the kept answer.
                if ( companion && mode.Pass == MeshPass::Depth )
                    RecordPrepassCellDraw( PrepassCellKey{ mat, MeshVertexPath::Static, twoSided, true },
                                           at->Draw );
                return at->Draw;
            }
            const uint32_t standInsBefore = m_StandInDraws;
            const CellDraw draw           = cellDrawOf( mat, twoSided, /*perObject*/ true );
            entries.insert( at, StaticCellDrawEntry{ .Material = mat,
                                                     .TwoSided = twoSided,
                                                     .Draw     = draw,
                                                     .StandIns = m_StandInDraws - standInsBefore } );
            return draw;
        };
        if ( !recordCall ) // a Record call's groups were split by its Resolve call
        {
            std::vector<const StaticMeshRenderData*> standingIn = scratch.TakeList();
            for ( auto& [mat, objects] : groups )
            {
                if ( objects.empty() || IsTranslucent( mat ) )
                    continue;
                const bool twoSided = groupTwoSided( mat, objects );
                // The deferred early-Z half this group belongs to (DeferredHalfTakes): out of the other halves.
                if ( !DeferredHalfTakes( mode.Half, mat,
                                         PrepassCellKey{ mat, MeshVertexPath::Static, twoSided, true } ) )
                {
                    objects.clear();
                    continue;
                }
                const CellDraw draw = keptCellDrawOf( mat, twoSided );
                if ( draw == CellDraw::DefaultSurface )
                    standingIn.insert( standingIn.end(), objects.begin(), objects.end() );
                if ( draw != CellDraw::Own )
                    objects.clear(); // Nothing: before the reveal, said once by ChooseCellDraw
            }
            // The deferred early-Z pass draws no stand-in: a group whose cells are still in the driver keeps its
            // own clip and depth in the G-buffer's Rest half (its record is not Own).
            if ( !standingIn.empty() && standIn != nullptr && mode.Half != DeferredPrepassHalf::Depth )
                groups.emplace_back( standIn->Material.get(), std::move( standingIn ) );
            else
                scratch.GiveList( std::move( standingIn ) );
        }
        const DataDrivenMaterial* const standInKey = standIn != nullptr ? standIn->Material.get() : nullptr;

        // WHICH MATERIAL RECORDS A GROUP's SINGLES (the comment where the singles take it), each refusal said once
        // per material. A Record call takes its Resolve call's answer (StaticGroupPlan::DrawMat). Asked only of a
        // group whose plan keeps a single draw (R5), and @p singleCount() counts those singles for the refusal.
        // The once-sets are the function's, not the lambda's, because a generic lambda's statics are one per
        // instantiation.
        static bool                                          s_WarnedSecondUnowned = false;
        static std::unordered_set<const DataDrivenMaterial*> s_WarnedNoGBufferVariant;
        const auto drawMatOf = [&]( DataDrivenMaterial* mat, const auto& singleCount ) -> DataDrivenMaterial*
        {
            if ( mode.Pass == MeshPass::Forward )
                return ForwardVirtualCell( mat, mat, MeshVertexPath::Static, MeshPass::Forward );
            DataDrivenMaterial* drawMat   = nullptr;
            auto*               materials = Runtime::ResourceRegistry::GetMaterialService();
            if ( mat == standInKey )
            {
                // the stand-in's own cell of this pass, never the spare
                drawMat = standInDraw != nullptr ? standInDraw->Material.get() : nullptr;
            }
            else if ( materials->Owns( mat ) )
            {
                drawMat = cells.DepthEqual ? DepthEqualCell( mat, MeshVertexPath::Static )
                                           : materials->GetVariant( mat, MeshVertexPath::Static, mode.Pass );
            }
            else
            {
                // A material a RENDERER built rather than a `.demat` — in practice only
                // MeshECSSystem's default, standing in for a mesh whose slot did not resolve. It has
                // no asset, so the service has no sibling of it; this pass keeps one of its own. See
                // SetupGBufferPass for why one is enough, and this is the check that says so.
                if ( unownedServed != nullptr && unownedServed != mat )
                {
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
                drawMat       = mode.Pass == MeshPass::GBuffer ? m_GBufferUnownedMaterial.get()
                                                               : m_DepthUnownedMaterial.get();
            }
            if ( drawMat == nullptr )
            {
                // Not a quiet skip: the objects in this group simply would not appear in a deferred
                // scene, which reads as "my mesh is invisible" and not as "one material has no
                // G-buffer variant". Once per material, because this runs every frame.
                if ( s_WarnedNoGBufferVariant.insert( mat ).second )
                    LOG_ERROR( "[MeshRenderer] No (Static x GBuffer) material for a mesh material; its "
                               "{} object(s) are NOT drawn into the G-buffer and will be missing from "
                               "the deferred scene. MaterialService logged which shader refused.",
                               singleCount() );
            }
            return drawMat;
        };

        // WHAT A GROUP RECORDS (R5; GAP 1, UE FParallelMeshDrawCommandPass: the setup decides once, the record
        // replays). Its objects by (mesh, hidden mask): a mesh's batchable ones go per LOD level into one
        // instanced draw of the group's set when the level holds two, and everything else is a single draw - into
        // the group's plan @p plan (DrawBuildScratch::StaticGroupPlan), with @p slice's buckets. Made ONCE, by the
        // call that plans (Full or Resolve), from one read of each object's instance; a Record call packs and
        // records the plan and buckets nothing, so what its Resolve call lent, applied and marked Drawn is what it
        // records. A setup task runs it, one slice of groups each: it reads the group, the walk's masks and the
        // objects' instances, and writes @p plan and @p slice alone.
        // Whether a group's surface clips per pixel (Masked; ResolveBinnedPipeline's ditheredLod rule): planGroup's
        // @p masked - an object of it in a dithered LOD transition draws both levels as singles
        // (ComputeDitheredLOD, each its half of the dither); an opaque one switches at once.
        const auto clipsDitheredLod = []( const DataDrivenMaterial* material ) noexcept
        {
            return material != nullptr &&
                   material->GetSchema().Blend == Core::Formats::SurfaceBlendMode::Masked;
        };
        const auto planGroup = [&]( const std::vector<const StaticMeshRenderData*>& objects, const bool standsIn,
                                    const bool masked, DrawBuildScratch::StaticGroupPlan& plan,
                                    DrawBuildScratch::BucketScratch&   slice )
        {
            plan.Batches.clear();
            plan.Instances.clear();
            plan.Singles.clear();
            // Sub-group this material's objects by Mesh*; a sub-group of >= 2 identical meshes collapses into
            // one instanced draw. Singletons (and everything when instancing is off / wireframe) take the
            // classic per-object path below — which also preserves their individual material overrides.
            // Keyed by (mesh, hidden mask): a section draw of a multi-material mesh batches with the same
            // section of its twins, one mask for the whole instanced draw. The buckets are the slice's, reused
            // by index: no allocation per group in the steady state.
            auto& buckets     = slice.Buckets;
            slice.BucketCount = 0;
            // Up to this many objects the buckets are searched in order (a short scan beats filling a table).
            constexpr std::size_t kBucketScanMax = 16;
            // THE RETAINED PER-PRIMITIVE PLAN (StaticPlanRows; UE FCachedPassMeshDrawList): an own-surface group
            // whose records all draw their own HiddenSubmeshes (no walked mask) takes each record's instance (the
            // columns), its overrides / alone answer and its (mesh, hidden) bucket id from its row - an index, no
            // hash table, no section-surface lookup. The buckets come out in first-seen order either way.
            const StaticPlanRows&    rows     = m_StaticPlanRows;
            const StaticWalkColumns& columns  = m_StaticColumns;
            const std::size_t        rowCount = std::min( rows.Count(), columns.Count() );
            const auto               rowOf    = [queueBase]( const StaticMeshRenderData* obj ) noexcept
            { return static_cast<std::size_t>( obj - queueBase ); };
            bool fromRows = !standsIn && rowCount != 0u;
            for ( std::size_t oi = 0; fromRows && oi < objects.size(); ++oi )
            {
                const std::size_t at = rowOf( objects[oi] );
                fromRows             = at < rowCount && rows.Bucket[at] != StaticPlanRows::kNoBucket &&
                           ( at >= walked.At.size() || walked.At[at] == 0u );
            }
            const bool indexed = !fromRows && objects.size() > kBucketScanMax;
            if ( indexed )
            {
                std::size_t slots = 64;
                while ( slots < objects.size() * 2u )
                    slots <<= 1u;
                slice.BucketIndex.assign( slots, 0u );
            }
            const auto makeBucket = [&]( Desert::Mesh* mesh, const SubmeshMask hidden ) -> std::vector<ObjDraw>&
            {
                if ( slice.BucketCount == buckets.size() )
                    buckets.emplace_back();
                DrawBuildScratch::MeshBucket& made = buckets[slice.BucketCount++];
                made.first                         = std::make_pair( mesh, hidden );
                made.second.clear();
                return made.second;
            };
            const auto bucketFor = [&]( Desert::Mesh* mesh, const SubmeshMask hidden ) -> std::vector<ObjDraw>&
            {
                if ( !indexed )
                {
                    for ( std::size_t bi = 0; bi < slice.BucketCount; ++bi )
                        if ( buckets[bi].first.first == mesh && buckets[bi].first.second == hidden )
                            return buckets[bi].second;
                    return makeBucket( mesh, hidden );
                }
                // The shadow walk's key hash (ShadowBucketKeyHash): the same (mesh, hidden mask) key.
                const std::size_t      key   = ShadowBucketKeyHash{}( ShadowBucketKey{ mesh, hidden } );
                std::vector<uint32_t>& table = slice.BucketIndex;
                const std::size_t      mask  = table.size() - 1u;
                for ( std::size_t at = key & mask;; at = ( at + 1u ) & mask )
                {
                    const uint32_t slot = table[at];
                    if ( slot == 0u )
                    {
                        std::vector<ObjDraw>& made = makeBucket( mesh, hidden );
                        table[at]                  = static_cast<uint32_t>( slice.BucketCount );
                        return made;
                    }
                    DrawBuildScratch::MeshBucket& found = buckets[slot - 1u];
                    if ( found.first.first == mesh && found.first.second == hidden )
                        return found.second;
                }
            };

            // Each object's row is the scene material table's (SyncGpuScene); here only its instance and whether
            // it overrides anything (an overriding object leaves the batch) - decided when the instance's render
            // copy was written (MaterialPropertySet::HasOverrides), not by a scan of its properties per pass.
            // From the plan rows (fromRows, above) or, for any other group, per object as before.
            if ( fromRows )
            {
                if ( slice.BucketOfId.size() < rows.BucketCount )
                {
                    slice.BucketOfId.resize( rows.BucketCount, 0u );
                    slice.IdStamp.resize( rows.BucketCount, 0u );
                }
                if ( ++slice.Stamp == 0u ) // wrapped: every stamp is stale again
                {
                    std::fill( slice.IdStamp.begin(), slice.IdStamp.end(), 0u );
                    slice.Stamp = 1u;
                }
                for ( const auto* obj : objects )
                {
                    const std::size_t at    = rowOf( obj );
                    const uint8_t     flags = rows.Flags[at];
                    ObjDraw           od;
                    od.Obj            = obj;
                    od.Inst           = columns.Instances[at];
                    od.HasOverrides   = ( flags & StaticPlanRows::kHasOverrides ) != 0u;
                    od.Alone          = ( flags & StaticPlanRows::kAlone ) != 0u;
                    od.Hidden         = SubmeshMask{ obj->HiddenSubmeshes };
                    const uint32_t id = rows.Bucket[at];
                    if ( slice.IdStamp[id] != slice.Stamp )
                    {
                        slice.IdStamp[id] = slice.Stamp;
                        makeBucket( obj->Mesh, od.Hidden );
                        slice.BucketOfId[id] = static_cast<uint32_t>( slice.BucketCount - 1u );
                    }
                    buckets[slice.BucketOfId[id]].second.push_back( od );
                }
            }
            for ( std::size_t oi = 0; !fromRows && oi < objects.size(); ++oi )
            {
                const StaticMeshRenderData* const obj = objects[oi];
                ObjDraw                           od;
                od.Obj = obj;
                // A stand-in object is the default surface, not the material it waits for: its own instance, its
                // defaults row — the waiting material's overrides are not the stand-in's to show.
                od.Inst = standsIn ? standIn->Instance.get()
                                   : SectionSurface( obj->MaterialSlots.get(), obj->SectionSlot ).Instance;
                od.HasOverrides = od.Inst != nullptr && od.Inst->GetPropertySet().HasOverrides();
                od.Alone        = !obj->ReceiveShadows || !obj->CastContactShadow || od.HasOverrides;
                od.Hidden       = hiddenOf( obj );
                bucketFor( obj->Mesh, od.Hidden ).push_back( od );
            }

            for ( std::size_t bi = 0; bi < slice.BucketCount; ++bi )
            {
                Desert::Mesh* const         mesh   = buckets[bi].first.first;
                const SubmeshMask           hidden = buckets[bi].first.second;
                const std::vector<ObjDraw>& bucket = buckets[bi].second;
                // Per-object state a shared batch entry can't carry: hidden submeshes, the
                // shadow-receive opt-out, the contact-shadow cast opt-out (a batch pushes one
                // PrimitiveFlags, the default) and INSTANCE OVERRIDES — a batch shares one material
                // entry, so an overridden instance in it would silently render with someone
                // else's values. All of those take the per-object path.
                auto& batchable = slice.Batchable;
                batchable.clear();
                for ( const auto& od : bucket )
                {
                    if ( od.Alone )
                        plan.Singles.push_back( od );
                    else
                        batchable.push_back( od );
                }
                if ( plan.Set == nullptr || batchable.size() < 2 )
                {
                    plan.Singles.insert( plan.Singles.end(), batchable.begin(), batchable.end() );
                    continue;
                }
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
                auto&          levels   = slice.LodLevels;
                // A fading object's entry: no level of a batch (it is planned as two singles).
                constexpr uint32_t kFadingLevel = ~0u;
                levels.clear();
                levels.reserve( batchable.size() );
                for ( const auto& od : batchable )
                {
                    // A fading object of a Masked group draws two levels: a single each (below), never a batch.
                    if ( masked &&
                         ComputeDitheredLOD( od.Obj->Transform, od.Obj->Mesh, od.Obj->ForcedLOD, od.Obj->LODBias )
                              .IsDithered() )
                    {
                        levels.push_back( kFadingLevel );
                        plan.Singles.push_back( od );
                        continue;
                    }
                    levels.push_back( std::min(
                         ComputeLOD( od.Obj->Transform, od.Obj->Mesh, od.Obj->ForcedLOD, od.Obj->LODBias ),
                         maxLevel ) );
                }

                Geometry::DistinctLODs( levels, slice.DistinctLods );
                for ( const uint32_t level : slice.DistinctLods )
                {
                    if ( level == kFadingLevel )
                        continue; // the fading objects, already singles
                    const auto count = static_cast<uint32_t>( std::count( levels.begin(), levels.end(), level ) );
                    if ( count < 2 )
                    {
                        // A level with a single member is cheaper as a per-object draw, and that is
                        // the same threshold the batch itself is chosen by.
                        for ( std::size_t i = 0; i < batchable.size(); ++i )
                            if ( levels[i] == level )
                                plan.Singles.push_back( batchable[i] );
                        continue;
                    }
                    // No batchable object carries overrides (filtered above), so every instance of
                    // the batch genuinely shares the parent material's effective values: the first one's
                    // table row (a batch carries receivers only, so the row is the receiving one), or the
                    // default surface's for a stand-in group.
                    DrawBuildScratch::PlannedBatch batch;
                    batch.Mesh          = mesh;
                    batch.Hidden        = hidden;
                    batch.Level         = level;
                    batch.MaterialIndex = standsIn ? m_StandInMaterialRows[0] : batchable[0].Obj->MaterialRow;
                    batch.First         = static_cast<uint32_t>( plan.Instances.size() );
                    batch.Count         = count;
                    for ( std::size_t i = 0; i < batchable.size(); ++i )
                        if ( levels[i] == level )
                            plan.Instances.push_back( batchable[i].Obj );
                    plan.Batches.push_back( batch );
                }
            }
            // The singles' levels, decided here with the batches' (the record replays them, it decides nothing).
            // A Masked group's single in a dithered transition draws both levels (UE: both LODs of the batch,
            // each with its half of the dither): this draw the newer, an appended copy the older.
            const std::size_t singleCount = plan.Singles.size();
            for ( std::size_t si = 0; si < singleCount; ++si )
            {
                ObjDraw& single = plan.Singles[si];
                if ( !masked )
                {
                    single.Level = ComputeLOD( single.Obj->Transform, single.Obj->Mesh, single.Obj->ForcedLOD,
                                               single.Obj->LODBias );
                    continue;
                }
                const Geometry::LODMask lod = ComputeDitheredLOD( single.Obj->Transform, single.Obj->Mesh,
                                                                  single.Obj->ForcedLOD, single.Obj->LODBias );
                single.Level       = lod.Levels[1];
                single.LodFadeSide = lod.IsDithered() ? -1.0f : 0.0f;
                if ( !lod.IsDithered() )
                    continue;
                ObjDraw older     = single;
                older.Level       = lod.Levels[0];
                older.LodFadeSide = 1.0f;
                plan.Singles.push_back( older );
            }
        };
        // A plan's batches packed into its set (a Full or a Record call; a Resolve call packs nothing).
        const auto packBatches = [&]( const DrawBuildScratch::StaticGroupPlan& plan )
        {
            for ( const DrawBuildScratch::PlannedBatch& batch : plan.Batches )
            {
                InstancedDraw d;
                d.Mesh              = batch.Mesh;
                d.InstanceCount     = batch.Count;
                d.FirstInstance     = static_cast<uint32_t>( plan.Set->Transforms.size() );
                d.MaterialIndex     = batch.MaterialIndex;
                d.HiddenSubmeshMask = batch.Hidden;
                d.LodLevel          = batch.Level;
                for ( uint32_t i = 0; i < batch.Count; ++i )
                    plan.Set->Transforms.push_back( plan.Instances[batch.First + i]->Transform );
                plan.Set->Draws.push_back( d );
            }
        };

        // A Record call's groups are its Resolve call's, one plan each. A call that plans: serial, in group order,
        // each group's set (setForGroup makes the sets in the order the one loop made them); then the bucketing
        // on setup tasks, about WorkerCount() + 1 contiguous slices of about equal object counts (the joining
        // thread buckets one too), none below kBucketSliceMin objects. ParallelSetup 0 runs the slices inline, in
        // group order: the same plans.
        auto& plans = scratch.GroupPlans;
        if ( !recordCall )
        {
            plans.resize( groups.size() );
            std::size_t planned = 0; // the objects of the opaque groups
            DESERT_PROFILE_PHASES( planPhases );
            DESERT_PROFILE_PHASE( planPhases, "Mesh: Static group sets" ); // serial: setForGroup per group
            for ( std::size_t gi = 0; gi < groups.size(); ++gi )
            {
                DrawBuildScratch::StaticGroupPlan& plan = plans[gi];
                plan.Set                                = nullptr;
                plan.DrawMat                            = nullptr;
                plan.OneSided                           = nullptr;
                plan.TwoSided                           = nullptr;
                plan.Batches.clear();
                plan.Instances.clear();
                plan.Singles.clear();
                // A Translucent template's objects belong to the translucency pass alone (RenderGlassManual):
                // no opaque bucket, no instanced variant is asked of them.
                plan.Opaque = !groups[gi].second.empty() && !IsTranslucent( groups[gi].first );
                if ( !plan.Opaque )
                    continue;
                // Where this group's batches accumulate, and WITH WHICH MATERIAL they will be recorded.
                // Asked once per group rather than per mesh bucket: the answer depends on the material
                // alone, and the ISM loop below asks the same question of its own materials.
                plan.Set = setForGroup( groups[gi].first, false );
                planned += groups[gi].second.size();
            }
            DESERT_PROFILE_PHASE( planPhases, "Mesh: Static bucketing (tasks + join)" );
            constexpr std::size_t kBucketSliceMin = 128; // objects: below it a task costs more than it buckets
            const std::size_t     participants    = ::Common::JobSystem::Get().WorkerCount() + 1;
            const std::size_t     slices = std::clamp<std::size_t>( planned / kBucketSliceMin, 1u, participants );
            if ( scratch.BucketSlices.size() < slices )
                scratch.BucketSlices.resize( slices );
            ::Common::TaskGroup tasks( m_SceneRenderer->ParallelSetup() );
            std::size_t         first   = 0;
            std::size_t         reached = 0; // the opaque objects of groups [0, first)
            for ( std::size_t s = 0; s < slices && first < groups.size(); ++s )
            {
                // Slice s ends where the objects reach planned * (s + 1) / slices; the last one at the end.
                const bool        lastSlice = s + 1 == slices;
                const std::size_t target    = planned * ( s + 1 ) / slices;
                std::size_t       last      = first;
                while ( last < groups.size() && ( lastSlice || reached < target ) )
                {
                    if ( plans[last].Opaque )
                        reached += groups[last].second.size();
                    ++last;
                }
                if ( last == first )
                    continue;
                DrawBuildScratch::BucketScratch& slice = scratch.BucketSlices[s];
                tasks.Launch( "Mesh: Static bucketing slice",
                              [&groups, &plans, &planGroup, &clipsDitheredLod, standInKey, &slice, first,
                               last]() noexcept
                              {
                                  for ( std::size_t gi = first; gi < last; ++gi )
                                      if ( plans[gi].Opaque )
                                          planGroup( groups[gi].second, groups[gi].first == standInKey,
                                                     clipsDitheredLod( groups[gi].first ), plans[gi], slice );
                              } );
                first = last;
            }
            tasks.Join();
        }
        else if ( plans.size() != groups.size() )
        {
            static_cast<void>( MissInSetupTask( "the static group plans" ) );
            return 0;
        }

        for ( std::size_t gi = 0; gi < groups.size(); ++gi )
        {
            auto* const                        mat  = groups[gi].first;
            DrawBuildScratch::StaticGroupPlan& plan = plans[gi];
            if ( !plan.Opaque )
                continue;
            // A Resolve call plans what its Record call records and nothing more (UE FParallelMeshDrawCommandPass:
            // the setup prepares exactly what the parallel tasks record): the group's set is drawn when the plan
            // batches into it, and only when the plan keeps singles the material that records them (lent the
            // material table and given the frame's scene state here, serially) and that material's one- and
            // two-sided pipelines. A group whose objects all batch asks drawMatOf nothing, as the Full call never
            // did (R5): no twin refusal, no unowned spare for it.
            if ( resolveCall )
            {
                if ( !plan.Batches.empty() )
                    plan.Set->Drawn = true;
                if ( plan.Singles.empty() )
                    continue;
                DataDrivenMaterial* const drawMat = drawMatOf( mat, [&plan] { return plan.Singles.size(); } );
                if ( drawMat != nullptr && LendMaterialTable( *drawMat ) )
                {
                    DESERT_PROFILE_SCOPE( "Mesh: SharedSceneSetup (1x/group)" );
                    frameState.ApplyTo( static_cast<Material*>( drawMat ) );
                    plan.DrawMat  = drawMat;
                    plan.OneSided = CellPipeline( staticPassState, *drawMat );
                    plan.TwoSided = CullPermutation( plan.OneSided, /*twoSided*/ true );
                }
                continue;
            }

            packBatches( plan );
            if ( plan.Singles.empty() )
                continue;
            const auto& singles = plan.Singles;

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
            DataDrivenMaterial* drawMat = plan.DrawMat; // a Record call's, resolved by its Resolve call
            if ( !recordCall )
                drawMat = drawMatOf( mat, [&singles] { return singles.size(); } );
            if ( drawMat == nullptr )
                continue; // refused out loud by drawMatOf, or MaterialService named what it refused

            // ---- Classic per-object path (singletons / wireframe) ----
            // The material is read through the scene material table (GpuScene::AddMaterialRow): each object's row
            // was registered by SyncGpuScene, stable for as long as its instance is drawn and re-uploaded only when
            // a value changes, so the drawn material's Materials block is lent the table and each draw names its
            // row. No per-frame per-group refill: one source of the values, the row the cached draws name too.
            if ( !recordCall && !LendMaterialTable( *drawMat ) ) // a Record call's was lent by its Resolve call
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
            if ( !recordCall ) // a Record call's group was given the scene state by its Resolve call
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
                const bool        twoSided = inst != nullptr ? inst->IsTwoSided() : drawMat->IsTwoSided();
                GraphicsPipeline* pipeline = nullptr;
                if ( recordCall )
                    pipeline = twoSided ? plan.TwoSided : plan.OneSided;
                else
                    pipeline = CullPermutation( CellPipeline( staticPassState, *drawMat ), twoSided );
                // View pass (forward or G-buffer): World / PrevWorld come from the object's motion row; the push
                // carries the submesh transform RELATIVE to it (identity, RenderMesh multiplies the submesh's in).
                list.Add( { .Pipeline          = pipeline,
                            .Mesh              = obj->Mesh,
                            .Transform         = glm::mat4( 1.0f ),
                            .Material          = drawMat->GetMaterialExecutor(),
                            .HiddenSubmeshMask = singles[i].Hidden,
                            .LodLevel          = singles[i].Level,
                            .Push     = DrawPushFields{}
                                         .Transform( glm::mat4( 1.0f ) )
                                         .PrimitiveIndex( obj->GpuPrimitive )
                                         .MaterialIndex( materialRow )
                                         .PrimitiveFlags( PrimitiveFlagsOf( obj->CastContactShadow ) )
                                         .LodFadeSide( singles[i].LodFadeSide ) } );
            }
        }

        // UE-style Instanced Static Meshes (one entity = N instances): fold into the same instanced
        // accumulation as the auto-batched static meshes. Each ISM is one batch; its transforms come
        // straight from the component array (no per-instance ECS cost), appended to the shared SSBO.
        //
        // AND IT IS NOT SILENT WHEN IT CANNOT. The queue has no per-object fallback, so "instancing is
        // unavailable" means "these entities do not appear". §1.4: that is refused out loud, with the
        // pass it happened in and how many entities it cost, rather than substituted with nothing.
        if ( !recordCall && !instancingOn && !m_InstancedQueue.empty() )
        {
            static bool s_WarnedNoInstancedCell = false;
            if ( !s_WarnedNoInstancedCell )
            {
                s_WarnedNoInstancedCell = true;
                LOG_ERROR( "[MeshRenderer] {} Instanced Static Mesh entit(ies) are NOT drawn in the {} "
                           "pass: it has no usable instanced cell (pipeline {}, material {}, instance {}"
                           "). An ISM is one entity holding N transforms and has no per-object path to "
                           "fall back to, so nothing of it reaches the frame.",
                           m_InstancedQueue.size(), MeshPassName( mode.Pass ),
                           instancedPipeline ? "ok" : "MISSING", instancedMaterial ? "ok" : "MISSING",
                           instancedInstance ? "ok" : "MISSING" );
            }
        }

        if ( instancingOn && !recordCall ) // a Record call's ISMs were culled and packed by its Resolve call
        {
            // The deferred halves count into one total (DeclareGBufferDraws sums the Resolve calls'; the prepass
            // does not count).
            DESERT_PROFILE_SCOPE( "Mesh: ISM draws" );
            // THREE STEPS (RENDER-PARALLEL-SETUP; UE FParallelMeshDrawCommandPass: what a task needs is resolved
            // before the tasks, and each task writes only its own slot). Serial, in queue order: each ISM's
            // checks, its set (setFor / setForGroup) and its cull entry (LookUpIsmViewCull). On setup tasks: the
            // stale entries' culls, each entry written by one task (its key is every input of the field's own, and
            // a call looks an entry up once). Serial, in queue order: the packing from each ISM's entry. The sets
            // are made in the order the one loop made them and every ISM packs the same cull, so the sets,
            // instances and draws are the same; ParallelSetup 0 runs the culls inline, in queue order.
            const Geometry::LODView mainView{ camera->GetPosition(), camera->GetProjectionMatrix() };
            ++m_IsmViewCullCall;
            scratch.IsmQueued.clear();
            scratch.IsmStale.clear();
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
                         CpuWalkHidden( mainWalk, ism.Mesh, m_FieldPrimitives[field], ism.HiddenSubmeshes,
                                        scratch.WalkWords );
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
                const PrepassCellKey ismKey{ mat, MeshVertexPath::Static, ism.Material->IsTwoSided(), false };
                if ( !DeferredHalfTakes( mode.Half, mat, ismKey ) )
                    continue;
                // By GpuScene id (SurfaceSceneInstancesVariant) once its cell is Ready and this view's graph has
                // the scene's instance rows (ismCellOf): the pass uploads 4 bytes per drawn instance, the vertex
                // stage reads its transform from the scene. Until then the plain cell packs its transforms, and
                // the stand-in (the renderer's spare, until the plain cell is Ready) too.
                const IsmCellChoice ismCell = ismCellOf( mat, ism.Material->IsTwoSided() );
                if ( ismCell.Draw == CellDraw::Nothing )
                    continue;
                const bool ismStandsIn = ismCell.Draw == CellDraw::DefaultSurface;
                if ( ismStandsIn && mode.Half == DeferredPrepassHalf::Depth )
                    continue; // no stand-in in the early-Z pass: the Rest half draws it with its own depth
                const bool         ismSceneIds = !ismStandsIn && ismCell.SceneIds;
                InstancedBatchSet* ismSet =
                     ismStandsIn ? &setFor( instancedMaterial, instancedInstance, false )
                                 : setForGroup( mat, ismSceneIds );
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
                //
                // Visible instances and their levels, gathered in ONE pass over the component's array.
                // The LOD question is per instance for the same reason the visibility question is: the
                // batch is one entity but forty thousand placements, and the near ones and the far ones
                // of a single ISM are not the same object to a renderer.
                // Walked once per change of its inputs, not once per pass per frame (LookUpIsmViewCull).
                const IsmViewCullLookup cull = LookUpIsmViewCull( ism, frustum, mainView );
                // By id: the ISM's range of the view's ids (PrepareViewIsmIds), filled from this very cull. The
                // view's cull inputs cannot change inside its frame, so a range that is not this entry's is a
                // broken frame order, said once, and the ISM is not drawn rather than drawn with other instances.
                uint32_t idBase = kNoViewIsmIds;
                if ( ismSceneIds )
                {
                    const auto            field = static_cast<size_t>( &ism - m_InstancedQueue.data() );
                    const ViewIsmIdRange& range = m_ViewIsmIdRanges[field];
                    if ( cull.Stale || range.Entry != cull.Entry || range.Rebuilds != cull.Entry->Rebuilds ||
                         range.Base == kNoViewIsmIds )
                    {
                        if ( !m_ViewIsmIdsMismatchSaid )
                            LOG_ERROR( "[MeshRenderer] an ISM's view ids are not its cull's this frame (the cull "
                                       "changed after PrepareViewIsmIds); the ISM is not drawn by id" );
                        m_ViewIsmIdsMismatchSaid = true;
                        continue;
                    }
                    idBase = range.Base;
                }
                if ( cull.Stale )
                    scratch.IsmStale.push_back( { .Entry = cull.Entry, .Ism = &ism } );
                scratch.IsmQueued.push_back( { .Ism      = &ism,
                                               .Hidden   = ismHidden,
                                               .Mat      = mat,
                                               .Set      = ismSet,
                                               .Cull     = cull.Entry,
                                               .IdBase   = idBase,
                                               .StandsIn = ismStandsIn } );
            }

            // The stale entries' culls on setup tasks, each entry by one (RunStaleIsmViewCulls). After
            // PrepareViewIsmIds none is stale: it culled this frame's main view before any pass.
            RunStaleIsmViewCulls( scratch.IsmStale, frustum );

            for ( const DrawBuildScratch::QueuedIsm& queued : scratch.IsmQueued )
            {
                const InstancedMeshRenderData& ism         = *queued.Ism;
                DataDrivenMaterial* const      mat         = queued.Mat;
                InstancedBatchSet* const       ismSet      = queued.Set;
                const bool                     ismStandsIn = queued.StandsIn;
                const std::vector<glm::mat4>&  visible     = queued.Cull->Visible;
                if ( mode.Half != DeferredPrepassHalf::Depth )
                    ismInstances += static_cast<uint32_t>( visible.size() );
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

                // The instances go into the set's one array in every call (a Resolve call's ahead of the
                // auto-batched ones its Record call packs); a Resolve call keeps the draws apart, its Record call
                // puts them behind the auto-batched draws (the Full call's draw order).
                std::vector<glm::mat4>&      ismTransforms = ismSet->Transforms;
                std::vector<InstancedDraw>&  ismDraws      = resolveCall ? ismSet->IsmDraws : ismSet->Draws;
                // A view pass: the wind at the view's previous frame too (B.z), for the instances' velocity.
                const InstanceWindPush ismWind = PackViewInstanceWind( ism.Wind );
                // One draw per level, its instances one contiguous slice of the entry's level-ordered Visible
                // (OrderIsmViewCullByLevel): the same instances in the same order a per-level filter gave.
                for ( const IsmLevelRun& run : queued.Cull->Runs )
                {
                    InstancedDraw d;
                    d.Mesh          = ism.Mesh;
                    d.FirstInstance = static_cast<uint32_t>( ismTransforms.size() );
                    d.MaterialIndex = *materialIndex;
                    d.HiddenSubmeshMask = queued.Hidden;
                    d.LodLevel          = run.Level;
                    d.Wind              = ismWind;
                    d.PrimitiveFlags = PrimitiveFlagsOf( ism.CastContactShadow );
                    if ( ismSet->SceneIds )
                    {
                        // The level's slice of the ISM's range of the view's ids (PrepareViewIsmIds: the entry's
                        // level-ordered Indices, each FirstGpuInstance + index), uploaded once for every pass.
                        d.FirstInstance = queued.IdBase + run.First;
                    }
                    else
                    {
                        const auto first = visible.begin() + run.First;
                        ismTransforms.insert( ismTransforms.end(), first, first + run.Count );
                    }
                    d.InstanceCount = run.Count;
                    ismDraws.push_back( d );
                }
            }
        }

        // ---- Instanced path: upload the packed SSBOs ONCE (at final size) before recording any instanced
        // draw, so the descriptor points at the final VkBuffer (a later grow reallocates it). Scene data is
        // uploaded a single time for the whole frame; each batch is then one instanced draw call. ----
        if ( scratch.InstSetCount != 0 )
        {
            DESERT_PROFILE_SCOPE( "Mesh: Instanced Batches" );

            // The instanced vertex stage reads its model matrix from the InstanceTransforms SSBO, so the
            // per-draw transform is unused; it is named rather than repeated as a literal.
            const glm::mat4 unusedModelTransform( 1.0F );

            for ( std::size_t si = 0; si < scratch.InstSetCount; ++si )
            {
                InstancedBatchSet& set = *instSets[si];
                if ( resolveCall )
                {
                    // The recorder's state before its Record call packs a draw: lent the material table, given
                    // the frame's scene state, its pipelines resolved (FlushStaticUploads uploads the transforms
                    // after the join). Only a set its Record call draws through: a group's batch (Drawn, from
                    // the group's plan) or an ISM's; the Full call skips an empty set alike.
                    if ( !set.Drawn && set.IsmDraws.empty() )
                        continue;
                    set.Lent = LendMaterialTable( *set.Mat );
                    if ( !set.Lent )
                        continue;
                    frameState.ApplyTo( static_cast<Material*>( set.Mat ) );
                    set.Mat->SetPushMatrix( glm::mat4( 1.0f ) ); // unused by the instanced VS
                    set.OneSided = CellPipeline( instancedPipeline, *set.Mat );
                    set.TwoSided = CullPermutation( set.OneSided, /*twoSided*/ true );
                    continue;
                }
                if ( recordCall )
                {
                    // The Resolve call's ISM draws, behind the auto-batched ones (the Full call's draw order);
                    // their instances already lead the set's array (the Resolve call packed them there), so each
                    // draw's FirstInstance stands.
                    set.Draws.insert( set.Draws.end(), set.IsmDraws.begin(), set.IsmDraws.end() );
                }
                if ( set.Draws.empty() )
                    continue;

                if ( recordCall )
                {
                    if ( !set.Lent )
                        continue; // refused by its Resolve call's lend, said there
                }
                else
                {
                    // UPLOAD AT FINAL SIZE BEFORE THE FIRST DRAW OF THIS SET IS RECORDED — growing a storage
                    // buffer reallocates the VkBuffer under a draw already recorded against the old one. It
                    // is per SET and no longer per pass, and that is not a weakening: a set's buffers are
                    // only ever written here, once, and the draws that read them are recorded immediately
                    // after, before any other set touches its own. (A Record call's: FlushStaticUploads,
                    // before the pass is declared - MeshDrawList::Add keeps the material, not its buffers.)
                    UploadInstanceTransforms( set );
                    // The draws' rows are the scene material table's (SyncGpuScene).
                    if ( !LendMaterialTable( *set.Mat ) )
                        continue;

                    // The recorder, not the instance's parent: they differ on the G-buffer pass (skinned path).
                    frameState.ApplyTo( static_cast<Material*>( set.Mat ) );
                    set.Mat->SetPushMatrix( glm::mat4( 1.0f ) ); // unused by the instanced VS
                }
                const bool setTwoSided = set.Inst != nullptr ? set.Inst->IsTwoSided() : set.Mat->IsTwoSided();
                // Its cell reads the scene's instance rows (MeshDrawList binds them to the set's block).
                if ( set.SceneIds )
                    list.SetSceneInstances( m_FrameSceneInstances );

                for ( const auto& d : set.Draws )
                {
                    // set.Mat is the recorder (the material's (Instanced x pass) variant or the renderer's
                    // spare): its cell is the program, as it is the sets'. A refused cell pipeline is null and
                    // the list refuses it, the pass's error.
                    const GraphicsPipeline* instancedDrawPipeline = nullptr;
                    if ( recordCall )
                        instancedDrawPipeline = setTwoSided ? set.TwoSided : set.OneSided;
                    else
                        instancedDrawPipeline =
                             CullPermutation( CellPipeline( instancedPipeline, *set.Mat ), setTwoSided );
                    list.Add( { .Pipeline          = instancedDrawPipeline,
                                .Mesh              = d.Mesh,
                                .Transform         = unusedModelTransform,
                                .Material          = set.Mat->GetMaterialExecutor(),
                                .InstanceCount     = d.InstanceCount,
                                .FirstInstance     = d.FirstInstance,
                                .HiddenSubmeshMask = d.HiddenSubmeshMask,
                                .LodLevel          = d.LodLevel,
                                .Push              = DrawPushFields{}
                                             .MaterialIndex( d.MaterialIndex )
                                             .Wind( d.Wind )
                                             .PrimitiveFlags( d.PrimitiveFlags ) } );
                }
            }
        }

        // The GPU-generated sections (the landscape grass), through the same pass's instanced cells. On the deferred
        // early-Z pairing BuildGpuInstancedDraws pairs them itself (DeclareMaskedDepthDraws, m_MaskedPrepassed):
        // once, in the Rest half's plain G-buffer state.
        if ( instancingOn &&
             mode.Half == DeferredPrepassHalf::None ) // the Rest half: BuildRestGpuInstancedDraws
        {
            DESERT_PROFILE_SCOPE( "Mesh: GPU-instanced draws" );
            BuildGpuInstancedDraws( list, instancedPipeline, instancedPass, frameState,
                                    companion ? &*companion : nullptr, false );
        }
        if ( resolveCall )
            scratch.Planned = true;
        return ismInstances;
    }

    void MeshRenderer::BuildRestGpuInstancedDraws( MeshDrawList& list )
    {
        // BuildStaticDraws's gates for its tail (a queue to walk, a camera); the Rest half pairs with nothing,
        // so its instancing is its own cells'.
        if ( ( m_StaticQueue.empty() && m_InstancedQueue.empty() ) || m_SceneRenderer->GetMainCamera() == nullptr )
            return;
        const StaticPassCells cells = StaticCellsOf( MeshPass::GBuffer, /*depthEqual*/ false );
        if ( !cells.InstancingOn )
            return;
        const SceneFrameBinding frameState = CaptureFrameState( m_SceneRenderer->GetViewFrame() );
        DESERT_PROFILE_SCOPE( "Mesh: GPU-instanced draws" );
        BuildGpuInstancedDraws( list, cells.InstancedState, MeshPass::GBuffer, frameState, nullptr, false );
    }

    void MeshRenderer::UploadInstanceTransforms( InstancedBatchSet& set )
    {
        auto* const sb = set.Mat->Get<StorageBufferProperty>( MeshForwardShaderParameters::InstanceTransforms );
        if ( sb == nullptr )
            return;
        // An ISM's cell by GpuScene id reads uint ids from the same block (Vertex_Instanced.glslh): the view's
        // ids, uploaded once (PrepareViewIsmIds), lent - the cell's draws name their ranges of it.
        if ( set.SceneIds )
            sb->LendViewBuffer( m_ViewIsmIds );
        else
            sb->SetRawData( set.Transforms.data(),
                            static_cast<uint32_t>( set.Transforms.size() * sizeof( glm::mat4 ) ) );
    }

    void MeshRenderer::FlushStaticUploads( DrawBuildScratch& scratch )
    {
        // Serial, after the Record calls joined and before the pass is declared: each set's buffer at final size
        // (the draws keep their material, not its buffers - MeshDrawList::Add), in the sets' order.
        for ( std::size_t si = 0; si < scratch.InstSetCount; ++si )
        {
            InstancedBatchSet& set = *scratch.InstSets[si];
            if ( set.Lent && !set.Draws.empty() )
                UploadInstanceTransforms( set );
        }
    }

    MeshRenderer::StaticPassCells MeshRenderer::StaticCellsOf( const MeshPass pass, const bool depthEqual )
    {
        StaticPassCells cells;
        cells.Pass                 = pass;
        cells.DepthEqual           = depthEqual && pass == MeshPass::GBuffer;
        StandInCell* const standIn = StandIn( MeshVertexPath::Static, MeshPass::Forward );
        switch ( pass )
        {
            case MeshPass::GBuffer:
                if ( cells.DepthEqual )
                {
                    // After the deferred early-Z pass: EQUAL, no depth writes, the Equal half's own instanced spare.
                    cells.StaticState       = m_StaticGBufferEqualPipeline.get();
                    cells.StandInDraw       = StandIn( MeshVertexPath::Static, MeshPass::GBuffer );
                    cells.InstancedState    = m_InstancedGBufferEqualPipeline.get();
                    cells.InstancedSpare    = m_InstancedGBufferEqualMaterial.get();
                    cells.InstancedInstance = m_InstancedGBufferEqualInstance.get();
                    break;
                }
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

    void MeshRenderer::BuildSkinnedDraws( MeshDrawList& list, const DrawBuildMode& mode,
                                          DrawBuildScratch& scratch )
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
        // @p equal: the G-buffer after the deferred early-Z pass (DeferredPrepassHalf), EQUAL without depth writes.
        const auto skinnedPassState = [this]( const MeshPass pass, const bool equal ) -> GraphicsPipeline*
        {
            if ( pass == MeshPass::GBuffer )
                return equal ? m_SkinnedGBufferEqualPipeline.get() : m_SkinnedGBufferPipeline.get();
            return pass == MeshPass::Depth ? m_SkinnedDepthPipeline.get()
                                           : WireframePipelineOr( m_SkinnedPipeline.get() );
        };
        const MeshPass cellPass  = mode.Pass;
        const bool     equalHalf = mode.Half == DeferredPrepassHalf::Equal;
        // The Wireframe view's skinned twin writes its own depth, as the static and instanced ones: not prepassed.
        if ( cellPass == MeshPass::Depth && WireframeView() )
            return;
        GraphicsPipeline* pipeline = skinnedPassState( cellPass, equalHalf );
        if ( pipeline == nullptr )
            return; // SetupGBufferPass / SetupDepthPrepass / SetupSkinnedGeometryPass said which cell is missing
        // The forward path's other half of the depth prepass pairing, whose readiness is folded in (none under the
        // Wireframe view: the twin is not prepassed). The deferred early-Z pairing: the prepass's companion is the
        // G-buffer's equal state, the Equal half's the prepass; the Rest half pairs with nothing.
        std::optional<MeshPass> companionPass;
        if ( mode.Half == DeferredPrepassHalf::Depth )
            companionPass = MeshPass::GBuffer;
        else if ( equalHalf )
            companionPass = MeshPass::Depth;
        else if ( mode.Half == DeferredPrepassHalf::None && PairsWithPrepass( mode.Pass, /*wireframe*/ true ) )
            companionPass = cellPass == MeshPass::Depth ? MeshPass::Forward : MeshPass::Depth;
        // The companion's G-buffer is the equal one (only the deferred prepass pairs with the G-buffer).
        const bool companionEqual = mode.Half == DeferredPrepassHalf::Depth;

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
        // The groups live in the scratch across calls (DrawBuildScratch::SkinnedGroups): recycled here, their
        // lists keep their capacity.
        using SectionRef = DrawBuildScratch::SkinnedSectionRef;
        scratch.RecycleSkinnedGroups();
        auto&      groups   = scratch.SkinnedGroups;
        const auto groupFor = [&]( DataDrivenMaterial* mat ) -> std::vector<SectionRef>&
        {
            for ( auto& [m, v] : groups )
                if ( m == mat )
                    return v;
            groups.emplace_back( mat, scratch.TakeSkinnedList() );
            return groups.back().second;
        };
        // The group's material is the queue's (surface x Skinned x Forward) material on the forward pass and its
        // (Skinned x GBuffer) sibling of the same `.demat` on the G-buffer pass: the sets come from the cell that
        // is bound (DrawStaticMeshes says why). A renderer-built material has no sibling; it takes the pass's
        // spare cell material, as the static path's m_GBufferUnownedMaterial.
        auto* const materials    = Runtime::ResourceRegistry::GetMaterialService();
        const auto  materialIn = [&]( const MeshPass pass, DataDrivenMaterial* mat,
                                     const bool equal ) -> DataDrivenMaterial*
        {
            if ( pass == MeshPass::Forward )
                return ForwardVirtualCell( mat, mat, MeshVertexPath::Skinned, MeshPass::Forward );
            if ( materials->Owns( mat ) )
                return pass == MeshPass::GBuffer && equal ? DepthEqualCell( mat, MeshVertexPath::Skinned )
                                                          : materials->GetVariant( mat, MeshVertexPath::Skinned, pass );
            return pass == MeshPass::GBuffer ? m_SkinnedGBufferUnownedMaterial.get()
                                             : m_SkinnedDepthUnownedMaterial.get();
        };
        const auto passMaterial = [&]( DataDrivenMaterial* mat ) { return materialIn( cellPass, mat, equalHalf ); };
        for ( const auto& data : m_SkinnedQueue )
        {
            if ( data.Mesh == nullptr )
                continue;
            for ( const auto& section : data.Sections )
                if ( section.Material != nullptr && section.Instance != nullptr &&
                     CpuWalkHidden(
                          cellPass == MeshPass::Depth && mode.Half != DeferredPrepassHalf::Depth
                               ? UnbinnedWalkView( GpuDriven::MeshPass::DepthPrepass )
                               : BinnedWalkView( cellPass == MeshPass::Forward ? GpuDriven::MeshPass::Forward
                                                                               : GpuDriven::MeshPass::GBuffer,
                                                 kBinnedMainView ),
                          /*skinned: never a VG primitive*/ nullptr, data.GpuPrimitive, section.HiddenSubmeshes,
                          scratch.WalkWords ) )
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
        const auto standInPipelineOf = [&]( const MeshPass pass, const bool equal ) -> const GraphicsPipeline*
        {
            StandInCell* const      cell  = standInOf( pass );
            GraphicsPipeline* const state = skinnedPassState( pass, equal );
            return cell != nullptr && state != nullptr ? CellPipeline( state, *cell->Material ) : nullptr;
        };
        StandInCell* const            standIn         = standInOf( cellPass );
        const GraphicsPipeline* const standInPipeline = standInPipelineOf( cellPass, equalHalf );
        {
            std::vector<SectionRef> standingIn = scratch.TakeSkinnedList();
            for ( auto& [mat, objects] : groups )
            {
                if ( objects.empty() )
                    continue;
                bool twoSided = mat->IsTwoSided();
                for ( const auto& obj : objects )
                    twoSided = twoSided || obj.Section->Instance->IsTwoSided();
                DataDrivenMaterial* const source = objects.front().Section->Material;
                const PrepassCellKey      key{ source, MeshVertexPath::Skinned, twoSided, true };
                // The deferred early-Z half this group belongs to (DeferredHalfTakes): out of the other halves.
                if ( !DeferredHalfTakes( mode.Half, source, key ) )
                {
                    objects.clear();
                    continue;
                }
                CellDraw draw = ChooseCellDraw( pipeline, *mat, twoSided, standInPipeline );
                if ( companionPass )
                {
                    // Paired with the prepass (as the static path's cellDrawOf): the prepass folds the other
                    // pass's readiness in and records it, the base pass of the same frame takes the record.
                    const std::optional<CellDraw> decided =
                         cellPass == MeshPass::Forward || equalHalf ? PrepassCellDraw( key ) : std::nullopt;
                    if ( decided )
                        draw = *decided;
                    else
                    {
                        DataDrivenMaterial* const twin  = materialIn( *companionPass, source, companionEqual );
                        GraphicsPipeline* const   state = skinnedPassState( *companionPass, companionEqual );
                        const CellDraw            other =
                             twin != nullptr && state != nullptr
                                  ? ChooseCellDraw( state, *twin, twoSided,
                                                    standInPipelineOf( *companionPass, companionEqual ) )
                                  : CellDraw::Nothing;
                        if ( other == CellDraw::Nothing || draw == CellDraw::Nothing )
                            draw = CellDraw::Nothing;
                        else if ( other == CellDraw::DefaultSurface )
                            draw = CellDraw::DefaultSurface;
                        if ( cellPass == MeshPass::Depth )
                            RecordPrepassCellDraw( key, draw );
                    }
                }
                if ( draw == CellDraw::DefaultSurface )
                    standingIn.insert( standingIn.end(), objects.begin(), objects.end() );
                if ( draw != CellDraw::Own )
                    objects.clear(); // Nothing: before the reveal, said once by ChooseCellDraw
            }
            // No stand-in in the deferred early-Z pass (as the static path): the Rest half draws it with its own depth.
            if ( !standingIn.empty() && standIn != nullptr && mode.Half != DeferredPrepassHalf::Depth )
                groups.emplace_back( standIn->Material.get(), std::move( standingIn ) );
            else
                scratch.GiveSkinnedList( std::move( standingIn ) );
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
                            .Push              = DrawPushFields{}
                                         .Transform( glm::mat4( 1.0f ) )
                                         .PrimitiveIndex( obj.Obj->GpuPrimitive )
                                         .MaterialIndex( *materialRow ) } );
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

    void MeshRenderer::SyncProgramFacts() const
    {
        const uint64_t registry = Runtime::ResourceRegistry::GetShaderService()->RecompileGeneration();
        if ( m_ProgramFactsValid && m_ProgramFactsRegistry == registry )
            return;
        // A Register / Recompile / Clear may have added, dropped or replaced any program: every row is re-made.
        m_ProgramFacts.clear();
        m_MaterialPrograms.clear();
        m_ProgramFactsRegistry  = registry;
        m_ProgramFactsValid     = true;
        m_GenericProgramsResolved = false;
        for ( std::string& known : m_GenericProgramNames )
            known.clear(); // every Shader Override slot asks its name again (GenericProgramOf)
    }

    MeshRenderer::ProgramFacts& MeshRenderer::FactsOf( const Shader& shader ) const
    {
        SyncProgramFacts();
        const uint64_t code = shader.GetCodeGeneration();
        auto [row, fresh]   = m_ProgramFacts.try_emplace( shader.GetSerial() );
        ProgramFacts& facts = row->second;
        if ( !fresh && facts.CodeGeneration == code )
            return facts;
        // Made once per program and code generation: the only place this path asks names of the service.
        auto* const       shaders = Runtime::ResourceRegistry::GetShaderService();
        const std::string& name   = shader.GetName();
        facts                     = ProgramFacts{ .CodeGeneration = code };
        const auto& state         = shader.GetProgramMeta().State;
        facts.Opaque              = !state.Blend.value_or( false ) && state.DepthWrite.value_or( true );
        const auto isRegistered   = [shaders]( std::string_view cell )
        { return shaders->GetByName( std::string( cell ) ) != nullptr; };
        const auto cellOf = [&]( const MeshPass pass ) -> std::shared_ptr<Shader>
        {
            const std::optional<std::string> cell =
                 TemplateCellShader( name, MeshVertexPath::Static, pass, isRegistered );
            if ( !cell )
                return nullptr;
            return shaders->GetByName( *cell );
        };
        facts.GBufferCell     = cellOf( MeshPass::GBuffer );
        facts.DepthCell       = cellOf( MeshPass::Depth );
        facts.DepthNormalCell = cellOf( MeshPass::DepthNormal );
        facts.FrontLayerCell  = FrontLayerShaderFor( name );
        if ( facts.FrontLayerCell )
            facts.FrontLayerProgram = shaders->GetByName( *facts.FrontLayerCell );
        return facts;
    }

    MeshRenderer::ResolvedProgram MeshRenderer::ProgramOf( const DataDrivenMaterial& material ) const
    {
        SyncProgramFacts();
        const std::string& name = material.GetShaderName();
        auto [row, fresh]       = m_MaterialPrograms.try_emplace( &material );
        MaterialProgramRow& kept = row->second;
        // A compare, never a lookup: a different name at this address is another material built there.
        if ( fresh || kept.Name != name )
        {
            kept.Name         = name;
            kept.Program      = Runtime::ResourceRegistry::GetShaderService()->GetByName( name );
            kept.Variants     = {}; // another material at this address: its siblings are asked again
            kept.VariantAsked = {};
        }
        if ( kept.Program == nullptr )
            return {};
        return ResolvedProgram{ .Program = kept.Program, .Facts = &FactsOf( *kept.Program ) };
    }

    const MeshRenderer::ResolvedProgram& MeshRenderer::GenericProgramOf( const std::size_t index ) const
    {
        SyncProgramFacts();
        if ( !m_GenericProgramsResolved )
        {
            // Once per frame for the whole queue (SubmitGenericMesh / ClearQueues reset it). A Shader Override
            // record carries its template by name: it is resolved once per queue slot and kept with the name it
            // was resolved from (m_GenericProgramNames). The next frame's record at that slot - the producers
            // submit in a stable order - compares its name (no hash, no lookup) and keeps the program; only a
            // different name asks the service. SyncProgramFacts forgets every slot when RecompileGeneration
            // moves (a Register / Recompile / Clear may add, drop or replace the program under a name).
            auto* const shaders = Runtime::ResourceRegistry::GetShaderService();
            m_GenericPrograms.resize( m_GenericQueue.size() );
            m_GenericProgramNames.resize( m_GenericQueue.size() );
            for ( std::size_t gi = 0; gi < m_GenericQueue.size(); ++gi )
            {
                const GenericMeshRenderData& g     = m_GenericQueue[gi];
                ResolvedProgram&             kept  = m_GenericPrograms[gi];
                std::string&                 known = m_GenericProgramNames[gi];
                if ( g.SlotMaterial != nullptr )
                {
                    known.clear(); // no Shader Override name is kept for this slot (capacity stays)
                    kept = {};
                    if ( const auto* const material = dynamic_cast<const DataDrivenMaterial*>( g.SlotMaterial ) )
                        kept = ProgramOf( *material );
                    continue;
                }
                if ( known != g.ShaderName() ) // SubmitGenericMesh admits no empty override name
                {
                    known.assign( g.ShaderName() );
                    kept.Program = shaders->GetByName( g.ShaderName() );
                }
                // The facts by the program's serial (an integer hash; re-made only when its code moved).
                kept.Facts = kept.Program != nullptr ? &FactsOf( *kept.Program ) : nullptr;
            }
            // The texture-set keys with the programs: each record's once a frame, into strings that keep their
            // capacity (BuildGenericDraws' slices and RegisterGenericRows read them, never rebuild them).
            // Each slot's texture-set id is kept with its key: the new key is written into a scratch and
            // compared with the slot's (no hash); only a different key - or a slot not keyed yet - is swapped in
            // and interned, so equal keys answer one id and the callers keep their materials by it.
            m_GenericTextureKeys.resize( m_GenericQueue.size() );
            m_GenericTextureSets.resize( m_GenericQueue.size(), 0u );
            for ( std::size_t gi = 0; gi < m_GenericQueue.size(); ++gi )
            {
                MeshRendererDetail::WriteGenericTextureKey( m_GenericQueue[gi], m_GenericTextureKeyScratch,
                                                            m_GenericTextureOrder );
                std::string& key = m_GenericTextureKeys[gi];
                if ( m_GenericTextureSets[gi] != 0u && key == m_GenericTextureKeyScratch )
                    continue;
                key.swap( m_GenericTextureKeyScratch );
                const auto next          = static_cast<uint32_t>( m_GenericTextureSetIds.size() + 1u );
                const auto interned      = m_GenericTextureSetIds.try_emplace( key, next ).first;
                m_GenericTextureSets[gi] = interned->second;
            }
            m_GenericProgramsResolved = true;
        }
        return m_GenericPrograms[index];
    }

    const std::string& MeshRenderer::GenericTextureKeyOf( const std::size_t index ) const
    {
        static_cast<void>( GenericProgramOf( index ) ); // resolves the queue's programs and keys once a frame
        return m_GenericTextureKeys[index];
    }

    uint32_t MeshRenderer::GenericTextureSetOf( const std::size_t index ) const
    {
        static_cast<void>( GenericProgramOf( index ) ); // made with the keys once a frame
        return m_GenericTextureSets[index];
    }

    MeshRenderer::SliceProgram* MeshRenderer::SliceProgramOf( ProgramFacts&                  facts,
                                                              const std::shared_ptr<Shader>& program,
                                                              const uint32_t                 index )
    {
        SliceProgram& kept = facts.Slices[index];
        if ( kept.Asked )
            return kept.Program != nullptr ? &kept : nullptr;
        kept.Asked = true;
        // Asked once per template and code generation (FactsOf re-makes the facts, SyncProgramFacts forgets them
        // when RecompileGeneration moves, which every refusal-forgetting Register / Recompile / Clear does).
        const std::shared_ptr<Shader>* cell = nullptr;
        if ( index == kSliceGBufferCell )
            cell = &facts.GBufferCell;
        else if ( index == kSliceDepthCell )
            cell = &facts.DepthCell;
        else if ( index == kSliceDepthNormalGBuffer || index == kSliceDepthNormalPrepass )
            cell = &facts.DepthNormalCell;
        if ( cell != nullptr )
        {
            kept.Own     = true; // no permutation is asked of a cell here
            kept.Program = *cell;
        }
        else
        {
            kept.Permutation = ForwardShadowMaskVariant( *program, ( index & 1u ) != 0, ( index & 2u ) != 0,
                                                         ( index & 4u ) != 0 );
            kept.Own         = !kept.Permutation.IsDefault();
            if ( !kept.Own )
                kept.Program = program;
            else
            {
                auto* const shaders  = Runtime::ResourceRegistry::GetShaderService();
                auto        acquired = shaders->AcquireVariant( program->GetName(), kept.Permutation );
                if ( !acquired )
                {
                    if ( shaders->ClaimRefusalReport( program->GetName(), kept.Permutation ) )
                        LOG_ERROR( "[MeshRenderer] forward program '{}' not drawn: {}", program->GetName(),
                                   acquired.GetError() );
                    return nullptr;
                }
                kept.Program = acquired.ExtractValue();
            }
        }
        if ( kept.Program == nullptr )
            return nullptr;

        // The program's name - the cell's or the template's - and its permutation's defines.
        const std::shared_ptr<Shader>& named = cell != nullptr ? *cell : program;
        kept.Key                             = named->GetName();
        for ( const auto& define : kept.Permutation.Defines )
            std::format_to( std::back_inserter( kept.Key ), "|{}", define );
        if ( index == kSliceGBufferCell )
            return &kept; // drawn through CellPipeline: no spec of its own

        kept.Spec           = GenericPipelineSpec( kept.Program );
        kept.Spec.DebugName = std::format( kGenericMeshNameFormat, kept.Key );
        if ( index == kSliceDepthNormalGBuffer )
        {
            // Into the G-buffer: its target layout; the program writes depth and the normal slot only.
            kept.Spec.TargetLayout      = GBufferLayout();
            kept.Spec.DepthCompareOp    = DepthCompare::CloserOrEqual;
            kept.Spec.DepthWriteEnabled = true;
        }
        else if ( index == kSliceDepthCell || index == kSliceDepthNormalPrepass )
        {
            // Into the scene depth alone, the prepass's state (its normal output has no attachment).
            kept.Spec.TargetLayout      = SceneDepthLayout();
            kept.Spec.DepthCompareOp    = DepthCompare::Closer;
            kept.Spec.DepthWriteEnabled = true;
        }
        // A forward slice keeps GenericPipelineSpec's state: an opaque program is already CloserOrEqual against
        // the depth its prepass wrote, which is what the deferred forward slices asked of it.
        return &kept;
    }

    DataDrivenMaterial* MeshRenderer::SlotVariantOf( DataDrivenMaterial& material, const uint32_t index,
                                                     const MeshPass pass, const Core::ShaderVariant& permutation )
    {
        auto* const materials = Runtime::ResourceRegistry::GetMaterialService();
        const auto  found     = m_MaterialPrograms.find( &material );
        if ( found == m_MaterialPrograms.end() ) // never resolved through ProgramOf: the service is asked
            return materials->GetVariant( &material, MeshVertexPath::Static, pass, permutation );
        MaterialProgramRow& kept     = found->second;
        const uint32_t      revision = materials->BuildRevision( &material );
        if ( kept.VariantRevision != revision )
        {
            // The asset was rebuilt (Invalidate retired every sibling): every slice asks again.
            kept.VariantRevision = revision;
            kept.Variants        = {};
            kept.VariantAsked    = {};
        }
        if ( !kept.VariantAsked[index] )
        {
            kept.VariantAsked[index] = true;
            kept.Variants[index] = materials->GetVariant( &material, MeshVertexPath::Static, pass, permutation );
        }
        return kept.Variants[index];
    }

    MeshRenderer::TranslucentDraw* MeshRenderer::TranslucentDrawOf( const DataDrivenMaterial* const mat )
    {
        const ResolvedProgram resolved = ProgramOf( *mat );
        if ( resolved.Facts == nullptr )
            return TranslucentDrawFor( mat->GetShaderName() ); // refuses (said once) an unregistered cell
        const std::size_t virtualLocal = m_ForwardVirtualLocal ? 1u : 0u;
        ProgramFacts&     facts        = *resolved.Facts;
        if ( !facts.TranslucentAsked[virtualLocal] )
        {
            facts.Translucent[virtualLocal]      = TranslucentDrawFor( mat->GetShaderName() );
            facts.TranslucentAsked[virtualLocal] = true;
        }
        return facts.Translucent[virtualLocal];
    }

    MeshRenderer::TranslucentDraw* MeshRenderer::FrontLayerDrawOf( const DataDrivenMaterial* const mat )
    {
        const ResolvedProgram resolved = ProgramOf( *mat );
        if ( resolved.Facts == nullptr )
        {
            const std::optional<std::string> cell = FrontLayerShaderFor( mat->GetShaderName() );
            return cell ? FrontLayerDrawFor( *cell ) : nullptr;
        }
        ProgramFacts& facts = *resolved.Facts;
        if ( !facts.FrontLayerAsked )
        {
            facts.FrontLayer      = facts.FrontLayerCell ? FrontLayerDrawFor( *facts.FrontLayerCell ) : nullptr;
            facts.FrontLayerAsked = true;
        }
        return facts.FrontLayer;
    }

} // namespace Desert::Graphic::System
